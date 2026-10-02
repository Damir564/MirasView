#include "ModelCache.h"
#include "Vertex.h"
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint32_t kCacheMagic = 0x564B4D44; // "VKMD"
constexpr uint32_t kCacheVersion = 3;

// Written verbatim (including the implicit padding after version), so its layout is part of the file format.
struct ModelCacheHeader {
    uint32_t magic = kCacheMagic;
    uint32_t version = kCacheVersion;
    uint64_t vertexCount;
    uint64_t indexCount;
    uint64_t submeshCount;
    uint64_t textureCount;
};
static_assert(sizeof(ModelCacheHeader) == 40);

// Textures are always decoded to 4 channels regardless of the stored channel count.
size_t rgbaSize(const TextureData& tex) {
    return tex.width * tex.height * 4;
}

// Optional trailer after the textures, only written for IFC models. Caches without it
// simply end after the textures, so adding it did not require a version bump.
constexpr uint32_t kIfcSectionMagic = 0x53434649; // "IFCS"
// Bump when the section layout or the IFC loader's output changes, so stale caches re-import.
constexpr uint32_t kIfcSectionVersion = 1;

void writeU32(std::ostream& out, uint32_t v) {
    out.write(reinterpret_cast<const char*>(&v), sizeof(v));
}

void writeString(std::ostream& out, const std::string& s) {
    writeU32(out, static_cast<uint32_t>(s.size()));
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

void writeStrings(std::ostream& out, const std::vector<std::string>& list) {
    writeU32(out, static_cast<uint32_t>(list.size()));
    for (const auto& s : list) writeString(out, s);
}

void writeStringMap(std::ostream& out, const std::unordered_map<std::string, std::string>& map) {
    writeU32(out, static_cast<uint32_t>(map.size()));
    for (const auto& [k, v] : map) {
        writeString(out, k);
        writeString(out, v);
    }
}

bool readU32(std::istream& in, uint32_t& v) {
    return static_cast<bool>(in.read(reinterpret_cast<char*>(&v), sizeof(v)));
}

bool readString(std::istream& in, std::string& s) {
    uint32_t size = 0;
    // Guards against reading a corrupt length as a multi-gigabyte allocation.
    if (!readU32(in, size) || size > (1u << 26)) return false;
    s.resize(size);
    return size == 0 || static_cast<bool>(in.read(s.data(), size));
}

bool readStrings(std::istream& in, std::vector<std::string>& list) {
    uint32_t count = 0;
    if (!readU32(in, count)) return false;
    list.resize(count);
    for (auto& s : list)
        if (!readString(in, s)) return false;
    return true;
}

bool readStringMap(std::istream& in, std::unordered_map<std::string, std::string>& map) {
    uint32_t count = 0;
    if (!readU32(in, count)) return false;
    map.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string k, v;
        if (!readString(in, k) || !readString(in, v)) return false;
        map.emplace(std::move(k), std::move(v));
    }
    return true;
}

void writeIfcScene(std::ostream& out, const IfcScene& scene) {
    writeU32(out, kIfcSectionMagic);
    writeU32(out, kIfcSectionVersion);
    writeString(out, scene.schema);
    writeStrings(out, scene.roots);

    writeU32(out, static_cast<uint32_t>(scene.spatial.size()));
    for (const auto& [guid, node] : scene.spatial) {
        writeString(out, node.guid);
        writeString(out, node.type);
        writeString(out, node.name);
        writeString(out, node.longName);
        writeString(out, node.parentGuid);
        writeStringMap(out, node.data);
        writeStrings(out, node.childSpatialGuids);
        writeStrings(out, node.elementGuids);
    }

    writeU32(out, static_cast<uint32_t>(scene.elements.size()));
    for (const auto& [guid, element] : scene.elements) {
        writeString(out, element.guid);
        writeString(out, element.type);
        writeString(out, element.name);
        writeString(out, element.objectType);
        writeString(out, element.tag);
        writeString(out, element.storey);
        writeString(out, element.parentSpatialGuid);
        writeU32(out, element.typeInfo ? 1u : 0u);
        if (element.typeInfo) {
            writeString(out, element.typeInfo->guid);
            writeString(out, element.typeInfo->type);
            writeString(out, element.typeInfo->name);
            writeString(out, element.typeInfo->predefinedType);
        }
        writeStringMap(out, element.data);
        writeU32(out, static_cast<uint32_t>(element.submeshIndices.size()));
        for (std::size_t si : element.submeshIndices)
            writeU32(out, static_cast<uint32_t>(si));
    }
}

bool readIfcScene(std::istream& in, IfcScene& scene, std::size_t submeshCount) {
    uint32_t version = 0;
    if (!readU32(in, version) || version != kIfcSectionVersion) return false;
    if (!readString(in, scene.schema) || !readStrings(in, scene.roots)) return false;

    uint32_t spatialCount = 0;
    if (!readU32(in, spatialCount)) return false;
    scene.spatial.reserve(spatialCount);
    for (uint32_t i = 0; i < spatialCount; ++i) {
        IfcSpatialNode node;
        if (!readString(in, node.guid) || !readString(in, node.type) || !readString(in, node.name) ||
            !readString(in, node.longName) || !readString(in, node.parentGuid) || !readStringMap(in, node.data) ||
            !readStrings(in, node.childSpatialGuids) || !readStrings(in, node.elementGuids))
            return false;
        std::string key = node.guid;
        scene.spatial.emplace(std::move(key), std::move(node));
    }

    uint32_t elementCount = 0;
    if (!readU32(in, elementCount)) return false;
    scene.elements.reserve(elementCount);
    for (uint32_t i = 0; i < elementCount; ++i) {
        IfcElement element;
        uint32_t hasTypeInfo = 0;
        if (!readString(in, element.guid) || !readString(in, element.type) || !readString(in, element.name) ||
            !readString(in, element.objectType) || !readString(in, element.tag) || !readString(in, element.storey) ||
            !readString(in, element.parentSpatialGuid) || !readU32(in, hasTypeInfo))
            return false;
        if (hasTypeInfo) {
            IfcTypeInfo info;
            if (!readString(in, info.guid) || !readString(in, info.type) || !readString(in, info.name) ||
                !readString(in, info.predefinedType))
                return false;
            element.typeInfo = std::move(info);
        }
        uint32_t submeshIndexCount = 0;
        if (!readStringMap(in, element.data) || !readU32(in, submeshIndexCount)) return false;
        element.submeshIndices.resize(submeshIndexCount);
        for (auto& si : element.submeshIndices) {
            uint32_t v = 0;
            if (!readU32(in, v) || v >= submeshCount) return false;
            si = v;
        }
        std::string key = element.guid;
        scene.elements.emplace(std::move(key), std::move(element));
    }

    scene.rebuildSubmeshMaps(submeshCount);
    return true;
}

} // namespace

namespace ModelCache {

bool isValid(const std::string& sourcePath, const std::string& cachePath) {
    namespace fs = std::filesystem;
    if (!fs::exists(cachePath)) return false;
    if (!fs::exists(sourcePath)) return false;
    return fs::last_write_time(cachePath) > fs::last_write_time(sourcePath);
}

bool save(const std::string& cachePath, const Mesh& mesh) {
    std::ofstream file(cachePath, std::ios::binary);
    if (!file.is_open()) return false;

    ModelCacheHeader header{};
    header.vertexCount = mesh.vertices.size();
    header.indexCount = mesh.indices.size();
    header.submeshCount = mesh.submeshes.size();
    header.textureCount = mesh.textureData.size();

    file.write(reinterpret_cast<const char*>(&header), sizeof(header));

    if (header.vertexCount > 0)
        file.write(reinterpret_cast<const char*>(mesh.vertices.data()), header.vertexCount * sizeof(Vertex));
    if (header.indexCount > 0)
        file.write(reinterpret_cast<const char*>(mesh.indices.data()), header.indexCount * sizeof(uint32_t));
    if (header.submeshCount > 0)
        file.write(reinterpret_cast<const char*>(mesh.submeshes.data()), header.submeshCount * sizeof(SubmeshInfo));

    for (const auto& tex : mesh.textureData) {
        file.write(reinterpret_cast<const char*>(&tex.width), sizeof(int));
        file.write(reinterpret_cast<const char*>(&tex.height), sizeof(int));
        file.write(reinterpret_cast<const char*>(&tex.channels), sizeof(int));

        bool linear = tex.isLinear;
        file.write(reinterpret_cast<const char*>(&linear), sizeof(bool));

        size_t dataSize = rgbaSize(tex);
        if (tex.pixels) {
            file.write(reinterpret_cast<const char*>(tex.pixels), dataSize);
        }
        else {
            std::vector<unsigned char> white(dataSize, 255);
            file.write(reinterpret_cast<const char*>(white.data()), dataSize);
        }
    }

    if (mesh.ifcScene)
        writeIfcScene(file, *mesh.ifcScene);

    return static_cast<bool>(file);
}

bool load(const std::string& cachePath, Mesh& outMesh) {
    std::ifstream file(cachePath, std::ios::binary);
    if (!file.is_open()) return false;

    ModelCacheHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (header.magic != kCacheMagic || header.version != kCacheVersion) return false;

    outMesh.vertices.resize(header.vertexCount);
    outMesh.indices.resize(header.indexCount);
    outMesh.submeshes.resize(header.submeshCount);
    outMesh.textureData.resize(header.textureCount);

    if (header.vertexCount > 0)
        file.read(reinterpret_cast<char*>(outMesh.vertices.data()), header.vertexCount * sizeof(Vertex));
    if (header.indexCount > 0)
        file.read(reinterpret_cast<char*>(outMesh.indices.data()), header.indexCount * sizeof(uint32_t));
    if (header.submeshCount > 0)
        file.read(reinterpret_cast<char*>(outMesh.submeshes.data()), header.submeshCount * sizeof(SubmeshInfo));

    for (TextureData& tex : outMesh.textureData) {
        file.read(reinterpret_cast<char*>(&tex.width), sizeof(int));
        file.read(reinterpret_cast<char*>(&tex.height), sizeof(int));
        file.read(reinterpret_cast<char*>(&tex.channels), sizeof(int));

        bool linear;
        file.read(reinterpret_cast<char*>(&linear), sizeof(bool));
        tex.isLinear = linear;

        size_t dataSize = rgbaSize(tex);
        // malloc (not stbi) so TextureData::free knows to release it with ::free via fromCache.
        tex.pixels = static_cast<unsigned char*>(malloc(dataSize));
        tex.fromCache = true;
        file.read(reinterpret_cast<char*>(tex.pixels), dataSize);
    }
    if (!file) return false;

    uint32_t sectionMagic = 0;
    if (!readU32(file, sectionMagic))
        return true; // No IFC section.
    if (sectionMagic != kIfcSectionMagic) return false;
    IfcScene scene;
    if (!readIfcScene(file, scene, outMesh.submeshes.size())) return false;
    outMesh.ifcScene = std::move(scene);
    return true;
}

} // namespace ModelCache
