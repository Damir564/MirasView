#include "ModelLoader.h"
#include "ModelCache.h"
#include "IfcDirectLoader.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <glm/glm.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include "stb_image.h"
#include "Log.h"

namespace {

using TextureCache = std::unordered_map<std::string, int>;

// Returns the texture index for key, creating the entry through prepare() on first use.
// Returns -1 when prepare() yields neither a path nor embedded data.
template <typename Prepare>
int findOrAddTexture(Mesh& result, TextureCache& cache, const std::string& key, bool isLinear, Prepare&& prepare) {
    if (auto it = cache.find(key); it != cache.end())
        return it->second;

    TextureData tex = prepare();
    if (tex.path.empty() && tex.encodedData == nullptr)
        return -1;

    tex.isLinear = isLinear;
    int newIdx = static_cast<int>(result.textureData.size());
    result.textureData.push_back(tex);
    cache[key] = newIdx;
    return newIdx;
}

void decodeTextureParallel(TextureData& tex) {
    if (tex.pixels != nullptr) {
        return;
    }

    if (tex.encodedData != nullptr && tex.encodedSize > 0) {
        tex.pixels = stbi_load_from_memory(
            tex.encodedData,
            static_cast<int>(tex.encodedSize),
            &tex.width, &tex.height, &tex.channels, 4);
    }
    else if (!tex.path.empty()) {
        std::string normalizedPath = tex.path;
        std::replace(normalizedPath.begin(), normalizedPath.end(), '\\', '/');

        tex.pixels = stbi_load(
            normalizedPath.c_str(),
            &tex.width, &tex.height, &tex.channels, 4);

        // Exporters often reference textures with the wrong extension case or format.
        if (!tex.pixels) {
            std::filesystem::path p(normalizedPath);
            std::string ext = p.extension().string();
            std::string lowerExt = ext;
            std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), ::tolower);

            if (ext != lowerExt) {
                std::string altPath = p.parent_path().string() + "/" +
                    p.stem().string() + lowerExt;
                tex.pixels = stbi_load(altPath.c_str(), &tex.width, &tex.height, &tex.channels, 4);
            }

            if (!tex.pixels) {
                std::vector<std::string> extensions = { ".png", ".jpg", ".jpeg", ".tga", ".bmp" };
                std::string basePath = p.parent_path().string() + "/" + p.stem().string();

                for (const auto& tryExt : extensions) {
                    std::string altPath = basePath + tryExt;
                    tex.pixels = stbi_load(altPath.c_str(), &tex.width, &tex.height, &tex.channels, 4);
                    if (tex.pixels) {
                        LOG_INFO("  Found texture at: " << altPath << "\n");
                        break;
                    }
                }
            }
        }
    }

    if (!tex.pixels) {
        LOG_ERROR("Texture failed to load: " << (tex.path.empty() ? "Embedded" : tex.path) << "\n");
        // 1x1 magenta so missing textures are obvious in the viewport.
        tex.width = 1;
        tex.height = 1;
        tex.channels = 4;
        tex.pixels = static_cast<unsigned char*>(malloc(4));
        if (tex.pixels) {
            tex.pixels[0] = 255;
            tex.pixels[1] = 0;
            tex.pixels[2] = 255;
            tex.pixels[3] = 255;
        }
    }
}

// Must only run once textureData is final: the async jobs hold references into it.
void decodeAllTextures(std::vector<TextureData>& textures, bool logProgress) {
    if (textures.empty())
        return;

    LOG_INFO("Decoding " << textures.size() << " textures in parallel...\n");

    std::vector<std::future<void>> futures;
    futures.reserve(textures.size());

    std::atomic<int> loadedCount{ 0 };
    const int totalTextures = static_cast<int>(textures.size());

    for (auto& tex : textures) {
        futures.push_back(std::async(std::launch::async, [&tex, &loadedCount, totalTextures, logProgress]() {
            decodeTextureParallel(tex);
            if (!logProgress)
                return;
            int loaded = ++loadedCount;
            if (loaded % 10 == 0 || loaded == totalTextures) {
                LOG_INFO("  Texture progress: " << loaded << "/" << totalTextures << "\n");
            }
            }));
    }

    for (auto& f : futures) {
        f.wait();
    }

    if (logProgress)
        LOG_INFO("All textures decoded.\n");
}

// ---------------------------------------------------------------------------------------------
// fastgltf (glTF / GLB)
// ---------------------------------------------------------------------------------------------

// Only records where the encoded image lives; decoding happens later in parallel.
// Embedded data points into the asset's buffers, so the asset must outlive decoding.
TextureData prepareTextureInfo(const fastgltf::Asset& asset, const fastgltf::Image& image, const std::string& modelPath) {
    TextureData texData{};

    std::visit(fastgltf::visitor{
        [&](const fastgltf::sources::URI& uri) {
            std::string pathString = std::string(uri.uri.path());
            texData.path = (std::filesystem::path(modelPath).parent_path() / pathString).string();
        },
        [&](const fastgltf::sources::Array& array) {
            texData.encodedData = reinterpret_cast<const unsigned char*>(array.bytes.data());
            texData.encodedSize = array.bytes.size();
        },
        [&](const fastgltf::sources::BufferView& view) {
            auto& bufferView = asset.bufferViews[view.bufferViewIndex];
            auto& buffer = asset.buffers[bufferView.bufferIndex];

            std::visit(fastgltf::visitor{
                [&](const fastgltf::sources::Vector& bufferVector) {
                    texData.encodedData = reinterpret_cast<const unsigned char*>(bufferVector.bytes.data() + bufferView.byteOffset);
                    texData.encodedSize = bufferView.byteLength;
                },
                [&](const fastgltf::sources::Array& bufferArray) {
                    texData.encodedData = reinterpret_cast<const unsigned char*>(bufferArray.bytes.data() + bufferView.byteOffset);
                    texData.encodedSize = bufferView.byteLength;
                },
                [](auto&) {}
            }, buffer.data);
        },
        [](auto&) {}
        }, image.data);

    return texData;
}

glm::mat4 getTransformMatrix(const fastgltf::Node& node, const glm::mat4& parentMatrix) {
    glm::mat4 localMatrix(1.0f);

    std::visit(fastgltf::visitor{
        [&](const fastgltf::math::fmat4x4& matrix) {
            memcpy(&localMatrix, matrix.data(), sizeof(float) * 16);
        },
        [&](const fastgltf::TRS& trs) {
            glm::vec3 translation(trs.translation[0], trs.translation[1], trs.translation[2]);
            // glm::quat takes (w, x, y, z); glTF stores (x, y, z, w).
            glm::quat rotation(trs.rotation[3], trs.rotation[0], trs.rotation[1], trs.rotation[2]);
            glm::vec3 scale(trs.scale[0], trs.scale[1], trs.scale[2]);

            glm::mat4 mT = glm::translate(glm::mat4(1.0f), translation);
            glm::mat4 mR = glm::mat4_cast(rotation);
            glm::mat4 mS = glm::scale(glm::mat4(1.0f), scale);

            localMatrix = mT * mR * mS;
        }
        }, node.transform);

    return parentMatrix * localMatrix;
}

// Returns -1 if the texture slot references no image.
int findOrAddGltfTexture(const fastgltf::Asset& asset, size_t textureIndex, const char* keyPrefix, bool isLinear,
    Mesh& result, TextureCache& textureCache, const std::string& path) {
    const auto& imageIndex = asset.textures[textureIndex].imageIndex;
    if (!imageIndex.has_value())
        return -1;

    size_t imgIdx = imageIndex.value();
    return findOrAddTexture(result, textureCache, keyPrefix + std::to_string(imgIdx), isLinear,
        [&] { return prepareTextureInfo(asset, asset.images[imgIdx], path); });
}

void readGltfMaterial(const fastgltf::Asset& asset, const fastgltf::Material& material,
    Mesh& result, TextureCache& textureCache, const std::string& path, Material& outMaterial) {
    const auto& pbr = material.pbrData;
    auto baseColor = pbr.baseColorFactor;

    outMaterial.baseColorFactor = glm::vec4(baseColor.x(), baseColor.y(), baseColor.z(), baseColor.w());
    outMaterial.metallicFactor = pbr.metallicFactor;
    outMaterial.roughnessFactor = pbr.roughnessFactor;

    switch (material.alphaMode) {
    case fastgltf::AlphaMode::Opaque:
        outMaterial.alphaMode = AlphaMode::OPAQUE;
        break;
    case fastgltf::AlphaMode::Mask:
        outMaterial.alphaMode = AlphaMode::MASK;
        outMaterial.alphaCutoff = material.alphaCutoff;
        break;
    case fastgltf::AlphaMode::Blend:
        outMaterial.alphaMode = AlphaMode::BLEND;
        break;
    }

    // Some exporters mark translucent materials as opaque.
    if (outMaterial.alphaMode == AlphaMode::OPAQUE && outMaterial.baseColorFactor.a < 0.99f) {
        outMaterial.alphaMode = AlphaMode::BLEND;
    }

    if (pbr.baseColorTexture.has_value()) {
        int idx = findOrAddGltfTexture(asset, pbr.baseColorTexture.value().textureIndex, "base:", false, result, textureCache, path);
        if (idx >= 0) outMaterial.baseColorTextureIndex = idx;
    }
    if (material.normalTexture.has_value()) {
        int idx = findOrAddGltfTexture(asset, material.normalTexture.value().textureIndex, "norm:", true, result, textureCache, path);
        if (idx >= 0) outMaterial.normalTextureIndex = idx;
    }
    if (pbr.metallicRoughnessTexture.has_value()) {
        int idx = findOrAddGltfTexture(asset, pbr.metallicRoughnessTexture.value().textureIndex, "mr:", true, result, textureCache, path);
        if (idx >= 0) outMaterial.metallicRoughnessTextureIndex = idx;
    }
}

// Appends one primitive to result, baked into world space. Returns false if it has no positions.
bool appendGltfPrimitive(const fastgltf::Asset& asset, const fastgltf::Primitive& primitive,
    const glm::mat4& globalTransform, const glm::mat3& normalMatrix,
    Mesh& result, TextureCache& textureCache, const std::string& path) {
    SubmeshInfo sub{};
    sub.vertexOffset = static_cast<uint32_t>(result.vertices.size());
    sub.indexOffset = static_cast<uint32_t>(result.indices.size());

    auto posAttrIt = primitive.findAttribute("POSITION");
    if (posAttrIt == primitive.attributes.end()) return false;
    auto& posAccessor = asset.accessors[posAttrIt->accessorIndex];

    // Vertices and indices are written straight into the output arrays (no per-primitive temporaries).
    const size_t vCount = posAccessor.count;
    const size_t baseVertex = result.vertices.size();
    const size_t baseIndex = result.indices.size();

    if (primitive.indicesAccessor.has_value()) {
        auto& idxAccessor = asset.accessors[primitive.indicesAccessor.value()];
        sub.indexCount = static_cast<uint32_t>(idxAccessor.count);
        result.indices.resize(baseIndex + idxAccessor.count);
        uint32_t* dst = result.indices.data() + baseIndex;
        fastgltf::iterateAccessorWithIndex<std::uint32_t>(asset, idxAccessor, [dst](std::uint32_t idx, size_t i) {
            dst[i] = idx;
            });
    }
    else {
        sub.indexCount = static_cast<uint32_t>(vCount);
        result.indices.resize(baseIndex + vCount);
        std::iota(result.indices.begin() + baseIndex, result.indices.end(), 0u);
    }
    const uint32_t* localIndices = result.indices.data() + baseIndex;
    const size_t localIndexCount = sub.indexCount;

    result.vertices.resize(baseVertex + vCount);
    Vertex* verts = result.vertices.data() + baseVertex;
    for (size_t i = 0; i < vCount; ++i) {
        verts[i].normal = glm::vec3(0.0f);
        verts[i].texCoord = glm::vec2(0.0f);
        verts[i].tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, posAccessor,
        [verts](fastgltf::math::fvec3 v, size_t i) {
            verts[i].position = glm::vec3(v.x(), v.y(), v.z());
        });

    if (auto it = primitive.findAttribute("NORMAL"); it != primitive.attributes.end()) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(asset, asset.accessors[it->accessorIndex],
            [verts](fastgltf::math::fvec3 v, size_t i) {
                verts[i].normal = glm::vec3(v.x(), v.y(), v.z());
            });
    }
    else {
        // No normals in the file: average the face normals of adjacent triangles.
        for (size_t i = 0; i + 2 < localIndexCount; i += 3) {
            uint32_t i0 = localIndices[i];
            uint32_t i1 = localIndices[i + 1];
            uint32_t i2 = localIndices[i + 2];
            glm::vec3 faceNormal = glm::normalize(glm::cross(
                verts[i1].position - verts[i0].position,
                verts[i2].position - verts[i0].position));
            verts[i0].normal += faceNormal;
            verts[i1].normal += faceNormal;
            verts[i2].normal += faceNormal;
        }
        for (size_t i = 0; i < vCount; ++i) {
            glm::vec3& n = verts[i].normal;
            n = glm::length(n) > 0.0001f ? glm::normalize(n) : glm::vec3(0.0f, 1.0f, 0.0f);
        }
    }

    if (auto it = primitive.findAttribute("TEXCOORD_0"); it != primitive.attributes.end()) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec2>(asset, asset.accessors[it->accessorIndex],
            [verts](fastgltf::math::fvec2 v, size_t i) {
                verts[i].texCoord = glm::vec2(v.x(), v.y());
            });
    }

    if (auto it = primitive.findAttribute("TANGENT"); it != primitive.attributes.end()) {
        fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(asset, asset.accessors[it->accessorIndex],
            [verts](fastgltf::math::fvec4 v, size_t i) {
                verts[i].tangent = glm::vec4(v.x(), v.y(), v.z(), v.w());
            });
    }

    for (size_t i = 0; i < vCount; ++i) {
        Vertex& v = verts[i];
        v.position = glm::vec3(globalTransform * glm::vec4(v.position, 1.0f));
        v.normal = glm::normalize(normalMatrix * v.normal);

        glm::vec3 tXYZ = glm::vec3(v.tangent);
        if (glm::length(tXYZ) > 0.0001f)
            v.tangent = glm::vec4(glm::normalize(normalMatrix * tXYZ), v.tangent.w);
        else
            v.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
    }

    if (primitive.materialIndex.has_value()) {
        readGltfMaterial(asset, asset.materials[primitive.materialIndex.value()], result, textureCache, path, sub.material);
    }
    result.submeshes.push_back(sub);
    return true;
}

void processFastGltfNode(const fastgltf::Asset& asset, size_t nodeIndex, const glm::mat4& parentTransform,
    Mesh& result, TextureCache& textureCache, const std::string& path) {
    const auto& node = asset.nodes[nodeIndex];
    glm::mat4 globalTransform = getTransformMatrix(node, parentTransform);

    if (node.meshIndex.has_value()) {
        const auto& mesh = asset.meshes[node.meshIndex.value()];
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(globalTransform)));

        for (const auto& primitive : mesh.primitives) {
            appendGltfPrimitive(asset, primitive, globalTransform, normalMatrix, result, textureCache, path);
        }
    }

    for (size_t childIndex : node.children) {
        processFastGltfNode(asset, childIndex, globalTransform, result, textureCache, path);
    }
}

// Counts vertices/indices over the scene's node tree (meshes can be instanced by several nodes).
void calculateTotals(const fastgltf::Asset& asset, size_t nodeIndex, size_t& totalVertices, size_t& totalIndices) {
    const auto& node = asset.nodes[nodeIndex];
    if (node.meshIndex.has_value()) {
        for (const auto& primitive : asset.meshes[*node.meshIndex].primitives) {
            auto it = primitive.findAttribute("POSITION");
            if (it == primitive.attributes.end()) continue;
            size_t vCount = asset.accessors[it->accessorIndex].count;
            totalVertices += vCount;
            totalIndices += primitive.indicesAccessor.has_value()
                ? asset.accessors[*primitive.indicesAccessor].count : vCount;
        }
    }
    for (size_t child : node.children)
        calculateTotals(asset, child, totalVertices, totalIndices);
}

void computeAllSubmeshBounds(Mesh& mesh) {
    for (auto& sub : mesh.submeshes) {
        if (sub.indexCount == 0) {
            sub.boundsMin = glm::vec3(0.0f);
            sub.boundsMax = glm::vec3(0.0f);
            continue;
        }

        sub.boundsMin = glm::vec3(std::numeric_limits<float>::max());
        sub.boundsMax = glm::vec3(std::numeric_limits<float>::lowest());

        const uint32_t end = sub.indexOffset + sub.indexCount;
        for (uint32_t i = sub.indexOffset; i < end; ++i) {
            const uint32_t absIdx = mesh.indices[i] + sub.vertexOffset;
            const glm::vec3& pos = mesh.vertices[absIdx].position;
            sub.boundsMin = glm::min(sub.boundsMin, pos);
            sub.boundsMax = glm::max(sub.boundsMax, pos);
        }
    }
}

Mesh loadWithFastGltf(const std::string& path) {
    Mesh result;
    fastgltf::Parser parser;
    auto gltfFile = fastgltf::MappedGltfFile::FromPath(path);
    if (!gltfFile) throw std::runtime_error("Failed to load glTF file: " + path);

    auto gltfOptions = fastgltf::Options::LoadGLBBuffers | fastgltf::Options::LoadExternalBuffers;

    auto assetRet = parser.loadGltf(gltfFile.get(), std::filesystem::path(path).parent_path(), gltfOptions);
    if (auto error = assetRet.error(); error != fastgltf::Error::None) {
        throw std::runtime_error("Failed to parse: " + std::string(fastgltf::getErrorMessage(error)));
    }
    auto& asset = assetRet.get();

    if (asset.scenes.empty()) return result;
    const auto& scene = asset.scenes[asset.defaultScene.value_or(0)];

    size_t totalVerts = 0, totalIndices = 0;
    for (size_t nodeIndex : scene.nodeIndices)
        calculateTotals(asset, nodeIndex, totalVerts, totalIndices);
    result.vertices.reserve(totalVerts);
    result.indices.reserve(totalIndices);

    TextureCache textureCache;
    for (size_t nodeIndex : scene.nodeIndices) {
        processFastGltfNode(asset, nodeIndex, glm::mat4(1.0f), result, textureCache, path);
    }

    // Embedded textures point into the asset, so decode before it goes out of scope.
    decodeAllTextures(result.textureData, false);

    LOG_INFO("Loaded " << result.vertices.size() << " vertices, " << result.textureData.size() << " textures.\n");
    return result;
}

Mesh loadFromSource(const std::string& path, const std::string& ext) {
    if (ext == ".ifc")
        return loadIfcDirect(path);
    if (ext == ".gltf" || ext == ".glb")
        return loadWithFastGltf(path);
    throw std::runtime_error("Unsupported model format: " + path);
}

} // namespace

bool isBuiltinModelPath(const std::string& path) {
    return path.rfind("builtin:", 0) == 0;
}

namespace {

// Unit cube centered on the origin: 4 vertices per face so every face has its own normal and UVs.
Mesh generateCube() {
    struct Face {
        glm::vec3 normal;
        glm::vec3 tangent;
    };
    const Face faces[6] = {
        { { 1, 0, 0 }, { 0, 0, -1 } },
        { { -1, 0, 0 }, { 0, 0, 1 } },
        { { 0, 1, 0 }, { 1, 0, 0 } },
        { { 0, -1, 0 }, { 1, 0, 0 } },
        { { 0, 0, 1 }, { 1, 0, 0 } },
        { { 0, 0, -1 }, { -1, 0, 0 } },
    };

    Mesh mesh;
    mesh.vertices.reserve(24);
    mesh.indices.reserve(36);
    for (const Face& face : faces) {
        const glm::vec3 bitangent = glm::cross(face.normal, face.tangent);
        const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
        const glm::vec2 corners[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        for (const glm::vec2& uv : corners) {
            Vertex v{};
            v.position = 0.5f * (face.normal + (uv.x * 2.0f - 1.0f) * face.tangent + (1.0f - uv.y * 2.0f) * bitangent);
            v.normal = face.normal;
            v.texCoord = uv;
            v.tangent = glm::vec4(face.tangent, 1.0f);
            mesh.vertices.push_back(v);
        }
        // Counter-clockwise when seen from outside.
        for (uint32_t i : { 0u, 2u, 1u, 0u, 3u, 2u })
            mesh.indices.push_back(base + i);
    }

    SubmeshInfo sub{};
    sub.indexOffset = 0;
    sub.indexCount = static_cast<uint32_t>(mesh.indices.size());
    sub.vertexOffset = 0;
    sub.material.baseColorFactor = glm::vec4(1.0f);
    sub.material.metallicFactor = 0.0f;
    sub.material.roughnessFactor = 0.5f;
    sub.boundsMin = glm::vec3(-0.5f);
    sub.boundsMax = glm::vec3(0.5f);
    mesh.submeshes.push_back(sub);
    return mesh;
}

} // namespace

Mesh loadModelSmart(const std::string& path) {
    if (path == kBuiltinCubePath)
        return generateCube();
    if (isBuiltinModelPath(path))
        throw std::runtime_error("Unknown builtin model: " + path);

    const std::string cachePath = path + ".cache";
    const std::u8string u8Ext = std::filesystem::path(path).extension().u8string();
    std::string ext(u8Ext.begin(), u8Ext.end());
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ModelCache::isValid(path, cachePath)) {
        LOG_INFO("[CACHE] Found valid cache for: " << path << ". Loading... ");
        Mesh cached;
        // IFC metadata lives in the cache too; an IFC cache without it predates that and is re-imported.
        if (ModelCache::load(cachePath, cached) && (ext != ".ifc" || cached.ifcScene)) {
            LOG_INFO("OK\n");
            return cached;
        }
        LOG_INFO("Failed (corrupt or outdated), re-importing\n");
    }

    Mesh result = loadFromSource(path, ext);
    computeAllSubmeshBounds(result);
    ModelCache::save(cachePath, result);
    return result;
}
