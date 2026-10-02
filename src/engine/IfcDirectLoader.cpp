#include "IfcDirectLoader.h"
#include "Vertex.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <exception>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <glm/glm.hpp>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_thread.h>
#include <web-ifc/modelmanager/ModelManager.h>
#include <web-ifc/schema/ifc-schema.h>
#include "Log.h"

namespace {

namespace schema = webifc::schema;
using webifc::parsing::IfcLoader;
using webifc::parsing::IfcTokenType;
using PropertyMap = std::unordered_map<std::string, std::string>;

// web-ifc's boolean code keeps unsynchronised static counters, so concurrent imports are serialised.
std::mutex g_webIfcMutex;

// web-ifc recurses deeply (boolean cuts for openings, BVH builds). Its WASM build reserves 5 MB and
// Linux threads get 8 MB, but Windows threads default to 1 MB, which Debug builds overflow.
// SDL reserves (not commits) this, so a generous size is cheap.
constexpr Sint64 kWebIfcStackSize = 64ll * 1024 * 1024;

// web-ifc logs through its own spdlog sink, not LOG_*, so silence it where there is no console.
#ifdef NDEBUG
constexpr uint8_t kSpdlogLevel = 6; // off
#else
constexpr uint8_t kSpdlogLevel = 4; // error
#endif
constexpr int kMaxNesting = 8;

// Spatial structure types across IFC2x3, IFC4 and IFC4x3 (infrastructure facilities).
constexpr uint32_t kSpatialTypes[] = {
    schema::IFCPROJECT, schema::IFCSITE, schema::IFCBUILDING, schema::IFCBUILDINGSTOREY, schema::IFCSPACE,
    schema::IFCFACILITY, schema::IFCFACILITYPART, schema::IFCBRIDGE, schema::IFCBRIDGEPART,
    schema::IFCROAD, schema::IFCROADPART, schema::IFCRAILWAY, schema::IFCRAILWAYPART,
    schema::IFCMARINEFACILITY, schema::IFCMARINEPART, schema::IFCSPATIALZONE, schema::IFCEXTERNALSPATIALELEMENT,
};

std::string formatReal(double value)
{
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 10);
    return result.ec == std::errc() ? std::string(buffer, result.ptr) : std::string();
}

// Attribute access on the STEP tape. web-ifc's own accessors assume well-formed input: moving to a
// missing line is a silent no-op and reading past a line's last attribute runs into the next line,
// so every read is validated first and unexpected token types yield empty results.
class StepReader {
public:
    explicit StepReader(const IfcLoader& loader) : m_loader(loader) {}

    uint32_t lineType(uint32_t id) const
    {
        return m_loader.IsValidExpressID(id) ? m_loader.GetLineType(id) : 0;
    }

    std::string string(uint32_t id, uint32_t argument) const
    {
        if (!moveTo(id, argument) || m_loader.GetTokenType() != IfcTokenType::STRING)
            return {};
        m_loader.StepBack();
        return m_loader.GetDecodedStringArgument();
    }

    std::string enumeration(uint32_t id, uint32_t argument) const
    {
        if (!moveTo(id, argument) || m_loader.GetTokenType() != IfcTokenType::ENUM)
            return {};
        m_loader.StepBack();
        return std::string(m_loader.GetStringArgument());
    }

    // Any attribute as display text: typed values are unwrapped and lists joined with ", ".
    std::string value(uint32_t id, uint32_t argument) const
    {
        return moveTo(id, argument) ? readValue(0) : std::string();
    }

    uint32_t ref(uint32_t id, uint32_t argument) const
    {
        if (!moveTo(id, argument) || m_loader.GetTokenType() != IfcTokenType::REF)
            return 0;
        m_loader.StepBack();
        return m_loader.GetRefArgument();
    }

    // A single reference or a (possibly nested) set of references.
    std::vector<uint32_t> refs(uint32_t id, uint32_t argument) const
    {
        std::vector<uint32_t> result;
        if (!moveTo(id, argument))
            return result;
        const IfcTokenType token = m_loader.GetTokenType();
        m_loader.StepBack();
        if (token == IfcTokenType::REF) {
            result.push_back(m_loader.GetRefArgument());
        }
        else if (token == IfcTokenType::SET_BEGIN) {
            for (uint32_t offset : m_loader.GetSetArgument()) {
                if (m_loader.GetTokenType(offset) == IfcTokenType::REF)
                    result.push_back(m_loader.GetRefArgument(offset));
            }
        }
        return result;
    }

private:
    bool moveTo(uint32_t id, uint32_t argument) const
    {
        if (!m_loader.IsValidExpressID(id) || argument >= m_loader.GetNoLineArguments(id))
            return false;
        m_loader.MoveToLineArgument(id, argument);
        return true;
    }

    // Reads the value at the current tape position and leaves the tape just after it.
    std::string readValue(int depth) const
    {
        const IfcTokenType token = m_loader.GetTokenType();
        switch (token) {
        case IfcTokenType::STRING:
            m_loader.StepBack();
            return m_loader.GetDecodedStringArgument();
        case IfcTokenType::BINARY:
            m_loader.StepBack();
            return std::string(m_loader.GetStringArgument());
        case IfcTokenType::ENUM: {
            m_loader.StepBack();
            const std::string_view s = m_loader.GetStringArgument();
            if (s == "T") return "true";
            if (s == "F") return "false";
            if (s == "U") return "unknown";
            return std::string(s);
        }
        case IfcTokenType::REAL:
            m_loader.StepBack();
            return formatReal(m_loader.GetDoubleArgument());
        case IfcTokenType::INTEGER:
            m_loader.StepBack();
            return std::string(m_loader.GetStringArgument());
        case IfcTokenType::REF:
            m_loader.StepBack();
            return "#" + std::to_string(m_loader.GetRefArgument());
        case IfcTokenType::LABEL:
            // Typed value such as IFCLENGTHMEASURE(2.5): the type name, then the value in parentheses.
            m_loader.StepBack();
            m_loader.GetStringArgument();
            if (m_loader.GetTokenType() != IfcTokenType::SET_BEGIN) {
                m_loader.StepBack();
                return {};
            }
            return readList(depth + 1);
        case IfcTokenType::SET_BEGIN:
            return readList(depth + 1);
        default:
            // $ and * carry no payload.
            return {};
        }
    }

    // Reads the items of an already opened list up to and including its closing token.
    std::string readList(int depth) const
    {
        std::string joined;
        while (!m_loader.IsAtEnd()) {
            const IfcTokenType token = m_loader.GetTokenType();
            if (token == IfcTokenType::SET_END || token == IfcTokenType::LINE_END)
                break;
            if (token == IfcTokenType::EMPTY || token == IfcTokenType::UNKNOWN)
                continue;
            m_loader.StepBack();
            std::string item = depth < kMaxNesting ? readValue(depth) : std::string();
            if (item.empty())
                continue;
            if (!joined.empty())
                joined += ", ";
            joined += item;
        }
        return joined;
    }

    const IfcLoader& m_loader;
};

void readProperty(const StepReader& reader, uint32_t id, const std::string& prefix, PropertyMap& out, int depth)
{
    if (depth > kMaxNesting)
        return;
    const std::string name = reader.string(id, 0);
    if (name.empty())
        return;
    const std::string key = prefix + name;

    switch (reader.lineType(id)) {
    case schema::IFCPROPERTYSINGLEVALUE:
    case schema::IFCPROPERTYENUMERATEDVALUE:
    case schema::IFCPROPERTYLISTVALUE:
        out[key] = reader.value(id, 2);
        break;
    case schema::IFCPROPERTYBOUNDEDVALUE: {
        const std::string upper = reader.value(id, 2);
        const std::string lower = reader.value(id, 3);
        out[key] = lower.empty() ? upper : upper.empty() ? lower : lower + " .. " + upper;
        break;
    }
    case schema::IFCCOMPLEXPROPERTY:
        for (uint32_t child : reader.refs(id, 3))
            readProperty(reader, child, key + ".", out, depth + 1);
        break;
    case schema::IFCQUANTITYLENGTH:
    case schema::IFCQUANTITYAREA:
    case schema::IFCQUANTITYVOLUME:
    case schema::IFCQUANTITYCOUNT:
    case schema::IFCQUANTITYWEIGHT:
    case schema::IFCQUANTITYTIME:
    case schema::IFCQUANTITYNUMBER:
        out[key] = reader.value(id, 3);
        break;
    case schema::IFCPHYSICALCOMPLEXQUANTITY:
        for (uint32_t child : reader.refs(id, 2))
            readProperty(reader, child, key + ".", out, depth + 1);
        break;
    default:
        break;
    }
}

// Flattens property sets and quantity sets to "SetName.Property" keys, like the metadata extractor.
// Sets are shared by many objects, so each is parsed once.
class PropertyReader {
public:
    explicit PropertyReader(const StepReader& reader) : m_reader(reader) {}

    const PropertyMap& propertySet(uint32_t id)
    {
        auto [it, inserted] = m_sets.try_emplace(id);
        if (!inserted)
            return it->second;

        uint32_t listArgument = 0;
        switch (m_reader.lineType(id)) {
        case schema::IFCPROPERTYSET: listArgument = 4; break;
        case schema::IFCELEMENTQUANTITY: listArgument = 5; break;
        default: return it->second; // Predefined sets (e.g. IfcDoorLiningProperties) are not flattened.
        }
        std::string name = m_reader.string(id, 2);
        if (name.empty())
            name = "Unnamed set";
        for (uint32_t property : m_reader.refs(id, listArgument))
            readProperty(m_reader, property, name + ".", it->second, 0);
        return it->second;
    }

    const PropertyMap& material(uint32_t id)
    {
        auto [it, inserted] = m_materials.try_emplace(id);
        if (inserted) {
            std::vector<std::string> names;
            describeMaterial(id, names, it->second, 0);
            std::string joined;
            for (const auto& n : names)
                joined += (joined.empty() ? "" : ", ") + n;
            if (!joined.empty())
                it->second["Material.Name"] = joined;
        }
        return it->second;
    }

private:
    void addName(std::vector<std::string>& names, std::string name)
    {
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(std::move(name));
    }

    void describeMaterial(uint32_t id, std::vector<std::string>& names, PropertyMap& out, int depth)
    {
        if (depth > kMaxNesting)
            return;
        switch (m_reader.lineType(id)) {
        case schema::IFCMATERIAL:
            addName(names, m_reader.string(id, 0));
            break;
        case schema::IFCMATERIALLIST:
            for (uint32_t m : m_reader.refs(id, 0))
                describeMaterial(m, names, out, depth + 1);
            break;
        case schema::IFCMATERIALLAYERSETUSAGE:
        case schema::IFCMATERIALPROFILESETUSAGE:
        case schema::IFCMATERIALPROFILESETUSAGETAPERING:
            describeMaterial(m_reader.ref(id, 0), names, out, depth + 1);
            break;
        case schema::IFCMATERIALLAYERSET: {
            const std::string setName = m_reader.string(id, 1);
            if (!setName.empty())
                out["Material.Layer set"] = setName;
            int index = 1;
            for (uint32_t layer : m_reader.refs(id, 0)) {
                const std::string layerMaterial = m_reader.string(m_reader.ref(layer, 0), 0);
                const std::string thickness = m_reader.value(layer, 1);
                out["Material.Layer " + std::to_string(index++)] =
                    (layerMaterial.empty() ? std::string("(no material)") : layerMaterial) +
                    (thickness.empty() ? "" : ", " + thickness);
                addName(names, layerMaterial);
            }
            break;
        }
        case schema::IFCMATERIALLAYER:
        case schema::IFCMATERIALLAYERWITHOFFSETS:
            describeMaterial(m_reader.ref(id, 0), names, out, depth + 1);
            break;
        case schema::IFCMATERIALPROFILESET:
        case schema::IFCMATERIALCONSTITUENTSET:
            for (uint32_t part : m_reader.refs(id, 2))
                describeMaterial(part, names, out, depth + 1);
            break;
        case schema::IFCMATERIALPROFILE:
        case schema::IFCMATERIALPROFILEWITHOFFSETS:
        case schema::IFCMATERIALCONSTITUENT:
            describeMaterial(m_reader.ref(id, 2), names, out, depth + 1);
            break;
        default:
            break;
        }
    }

    const StepReader& m_reader;
    std::unordered_map<uint32_t, PropertyMap> m_sets;
    std::unordered_map<uint32_t, PropertyMap> m_materials;
};

// Relationship tables, gathered in one pass over the relationship lines.
struct Relationships {
    std::unordered_map<uint32_t, std::vector<uint32_t>> propertySets; // object -> property set definitions
    std::unordered_map<uint32_t, uint32_t> typeObject;                // occurrence -> type object
    std::unordered_map<uint32_t, uint32_t> material;                  // object or type -> material select
    std::unordered_map<uint32_t, uint32_t> container;                 // element -> spatial structure
    std::unordered_map<uint32_t, uint32_t> aggregateParent;           // part -> whole
    std::unordered_map<uint32_t, std::vector<uint32_t>> aggregateChildren;
};

Relationships readRelationships(const IfcLoader& loader, const StepReader& reader)
{
    Relationships rel;
    for (uint32_t id : loader.GetExpressIDsWithType(schema::IFCRELDEFINESBYPROPERTIES)) {
        // IFC4 allows a set of definitions here (IfcPropertySetDefinitionSet).
        const auto definitions = reader.refs(id, 5);
        for (uint32_t object : reader.refs(id, 4)) {
            auto& list = rel.propertySets[object];
            list.insert(list.end(), definitions.begin(), definitions.end());
        }
    }
    for (uint32_t id : loader.GetExpressIDsWithType(schema::IFCRELDEFINESBYTYPE)) {
        if (const uint32_t type = reader.ref(id, 5))
            for (uint32_t object : reader.refs(id, 4))
                rel.typeObject[object] = type;
    }
    for (uint32_t id : loader.GetExpressIDsWithType(schema::IFCRELASSOCIATESMATERIAL)) {
        if (const uint32_t material = reader.ref(id, 5))
            for (uint32_t object : reader.refs(id, 4))
                rel.material.try_emplace(object, material);
    }
    for (uint32_t id : loader.GetExpressIDsWithType(schema::IFCRELCONTAINEDINSPATIALSTRUCTURE)) {
        if (const uint32_t structure = reader.ref(id, 5))
            for (uint32_t element : reader.refs(id, 4))
                rel.container.try_emplace(element, structure);
    }
    for (uint32_t id : loader.GetExpressIDsWithType(schema::IFCRELAGGREGATES)) {
        const uint32_t whole = reader.ref(id, 4);
        if (!whole)
            continue;
        for (uint32_t part : reader.refs(id, 5)) {
            if (part == whole || !rel.aggregateParent.try_emplace(part, whole).second)
                continue;
            rel.aggregateChildren[whole].push_back(part);
        }
    }
    return rel;
}

// Occurrence properties win over the ones inherited from the type object, as in IfcOpenShell.
void collectObjectData(uint32_t id, const Relationships& rel, const StepReader& reader,
    PropertyReader& properties, PropertyMap& out)
{
    if (auto it = rel.propertySets.find(id); it != rel.propertySets.end())
        for (uint32_t set : it->second)
            for (const auto& [k, v] : properties.propertySet(set))
                out[k] = v;

    const auto typeIt = rel.typeObject.find(id);
    if (typeIt != rel.typeObject.end()) {
        for (uint32_t set : reader.refs(typeIt->second, 5))
            for (const auto& [k, v] : properties.propertySet(set))
                out.try_emplace(k, v);
    }

    auto materialIt = rel.material.find(id);
    if (materialIt == rel.material.end() && typeIt != rel.typeObject.end())
        materialIt = rel.material.find(typeIt->second);
    if (materialIt != rel.material.end())
        for (const auto& [k, v] : properties.material(materialIt->second))
            out.try_emplace(k, v);
}

std::optional<IfcTypeInfo> readTypeInfo(uint32_t typeId, const StepReader& reader, const schema::IfcSchemaManager& schemaManager)
{
    const uint32_t typeCode = reader.lineType(typeId);
    if (!typeCode)
        return std::nullopt;
    IfcTypeInfo info;
    info.guid = reader.string(typeId, 0);
    info.type = schemaManager.IfcTypeCodeToType(typeCode);
    info.name = reader.string(typeId, 2);
    // IfcElementType subtypes put PredefinedType right after ElementType; the IFC2x3 *Style
    // entities use that slot for something else.
    if (info.type.ends_with("Type"))
        info.predefinedType = reader.enumeration(typeId, 9);
    return info;
}

struct ColourGroup {
    glm::dvec4 colour;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

// Groups the element's pieces by colour; each group becomes one submesh.
void collectElementGeometry(webifc::geometry::IfcGeometryProcessor& processor,
    const webifc::geometry::IfcFlatMesh& flatMesh, std::vector<ColourGroup>& groups)
{
    constexpr size_t kStride = webifc::geometry::VERTEX_FORMAT_SIZE_FLOATS;
    std::vector<Vertex> pieceVertices;

    for (const auto& placed : flatMesh.geometries) {
        const auto& geom = processor.GetGeometry(placed.geometryExpressID);
        const size_t vertexCount = geom.vertexData.size() / kStride;
        if (vertexCount == 0 || geom.indexData.size() < 3)
            continue;

        const glm::dmat4& m = placed.transformation;
        const glm::dmat3 normalMatrix = glm::transpose(glm::inverse(glm::dmat3(m)));
        // Mirrored placements flip triangle orientation.
        const bool flip = glm::determinant(glm::dmat3(m)) < 0.0;

        pieceVertices.clear();
        pieceVertices.reserve(vertexCount);
        bool finite = true;
        for (size_t v = 0; v < vertexCount && finite; ++v) {
            const double* src = geom.vertexData.data() + v * kStride;
            const glm::dvec3 p = glm::dvec3(m * glm::dvec4(src[0], src[1], src[2], 1.0));
            glm::dvec3 n = normalMatrix * glm::dvec3(src[3], src[4], src[5]);
            finite = std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
            const double len = glm::length(n);
            Vertex vert{};
            vert.position = glm::vec3(p);
            vert.normal = std::isfinite(len) && len > 1e-12 ? glm::vec3(n / len) : glm::vec3(0.0f, 1.0f, 0.0f);
            vert.texCoord = glm::vec2(0.0f);
            vert.tangent = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
            pieceVertices.push_back(vert);
        }
        // A broken tessellation would poison the bounds used for picking and focusing.
        if (!finite)
            continue;

        glm::dvec4 colour = glm::clamp(placed.color, glm::dvec4(0.0), glm::dvec4(1.0));
        auto group = std::find_if(groups.begin(), groups.end(), [&](const ColourGroup& g) { return g.colour == colour; });
        if (group == groups.end())
            group = groups.insert(groups.end(), ColourGroup{ colour, {}, {} });

        const uint32_t base = static_cast<uint32_t>(group->vertices.size());
        const auto& idx = geom.indexData;
        for (size_t i = 0; i + 2 < idx.size(); i += 3) {
            if (idx[i] >= vertexCount || idx[i + 1] >= vertexCount || idx[i + 2] >= vertexCount)
                continue;
            group->indices.push_back(base + idx[i]);
            group->indices.push_back(base + (flip ? idx[i + 2] : idx[i + 1]));
            group->indices.push_back(base + (flip ? idx[i + 1] : idx[i + 2]));
        }
        group->vertices.insert(group->vertices.end(), pieceVertices.begin(), pieceVertices.end());
    }
}

// Appends the groups as submeshes; returns their indices.
std::vector<std::size_t> appendGroups(std::vector<ColourGroup>& groups, Mesh& result)
{
    std::vector<std::size_t> submeshIndices;
    for (ColourGroup& group : groups) {
        if (group.indices.empty())
            continue;
        SubmeshInfo sub{};
        sub.vertexOffset = static_cast<uint32_t>(result.vertices.size());
        sub.indexOffset = static_cast<uint32_t>(result.indices.size());
        sub.indexCount = static_cast<uint32_t>(group.indices.size());
        sub.material.baseColorFactor = glm::vec4(group.colour);
        sub.material.metallicFactor = 0.0f;
        sub.material.roughnessFactor = 0.8f;
        sub.material.alphaMode = group.colour.a < 0.99 ? AlphaMode::BLEND : AlphaMode::OPAQUE;

        result.vertices.insert(result.vertices.end(), group.vertices.begin(), group.vertices.end());
        result.indices.insert(result.indices.end(), group.indices.begin(), group.indices.end());
        submeshIndices.push_back(result.submeshes.size());
        result.submeshes.push_back(sub);
    }
    return submeshIndices;
}

std::string uniqueGuid(std::string guid, uint32_t id, const auto& existing)
{
    if (guid.empty() || existing.contains(guid))
        guid += "#" + std::to_string(id);
    return guid;
}

// Runs job on a thread whose stack is large enough for web-ifc, rethrowing its exception here.
void runWithLargeStack(const std::function<void()>& job)
{
    struct Context {
        const std::function<void()>* job;
        std::exception_ptr error;
    } context{ &job, nullptr };

    const SDL_ThreadFunction entry = [](void* data) -> int {
        auto* ctx = static_cast<Context*>(data);
        try {
            (*ctx->job)();
        }
        catch (...) {
            ctx->error = std::current_exception();
        }
        return 0;
    };

    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, reinterpret_cast<void*>(entry));
    SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "web-ifc import");
    SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, &context);
    SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, kWebIfcStackSize);
    SDL_Thread* thread = SDL_CreateThreadWithProperties(props);
    SDL_DestroyProperties(props);
    if (!thread)
        throw std::runtime_error(std::string("Could not start the IFC import thread: ") + SDL_GetError());
    SDL_WaitThread(thread, nullptr);
    if (context.error)
        std::rethrow_exception(context.error);
}

Mesh importIfc(std::istream& file, const std::string& ifcPath)
{
    LOG_INFO("[IFC direct] Parsing " << ifcPath << " with web-ifc...\n");

    webifc::manager::ModelManager manager(false);
    manager.SetLogLevel(kSpdlogLevel);
    webifc::manager::LoaderSettings settings;
    const uint32_t modelID = manager.CreateModel(settings);

    IfcLoader* loader = manager.GetIfcLoader(modelID);
    if (!loader->LoadFile(file) || loader->GetMaxExpressId() == 0)
        throw std::runtime_error("web-ifc could not parse: " + ifcPath);

    const auto& schemaManager = manager.GetSchemaManager();
    auto* processor = manager.GetGeometryProcessor(modelID);
    const StepReader reader(*loader);
    PropertyReader properties(reader);
    const Relationships rel = readRelationships(*loader, reader);

    Mesh result;
    IfcScene scene;
    scene.schema = std::string(schemaManager.GetSchemaName(loader->GetSchema()));

    // Geometry: one submesh per colour per element. Spaces and openings are skipped, as IfcConvert
    // does by default; other spatial elements (e.g. site terrain) are drawn when they have a shape.
    std::vector<uint32_t> productIds;
    for (uint32_t type : schemaManager.GetIfcElementList()) {
        if (type == schema::IFCOPENINGELEMENT || type == schema::IFCOPENINGSTANDARDCASE || type == schema::IFCSPACE)
            continue;
        const auto ids = loader->GetExpressIDsWithType(type);
        productIds.insert(productIds.end(), ids.begin(), ids.end());
    }
    // File order keeps the submesh layout and the hierarchy order stable between runs.
    std::sort(productIds.begin(), productIds.end());

    std::vector<std::pair<uint32_t, std::string>> elementGuids;
    std::vector<ColourGroup> groups;
    size_t failed = 0;
    for (uint32_t id : productIds) {
        const uint32_t type = reader.lineType(id);
        groups.clear();
        try {
            const auto flatMesh = processor->GetFlatMesh(id);
            collectElementGeometry(*processor, flatMesh, groups);
        }
        catch (const std::exception& e) {
            ++failed;
            LOG_ERROR("[IFC direct] Element #" << id << " failed: " << e.what() << "\n");
        }
        // Frees per-element geometry; mapped representations are shared between elements, so they stay.
        processor->Clear(true);

        std::vector<std::size_t> submeshIndices = appendGroups(groups, result);
        if (submeshIndices.empty())
            continue;

        IfcElement element;
        element.guid = uniqueGuid(reader.string(id, 0), id, scene.elements);
        element.type = schemaManager.IfcTypeCodeToType(type);
        element.name = reader.string(id, 2);
        element.objectType = reader.string(id, 4);
        element.tag = reader.string(id, 7);
        element.submeshIndices = std::move(submeshIndices);
        if (auto typeIt = rel.typeObject.find(id); typeIt != rel.typeObject.end())
            element.typeInfo = readTypeInfo(typeIt->second, reader, schemaManager);

        element.data["ExpressID"] = std::to_string(id);
        if (std::string description = reader.string(id, 3); !description.empty())
            element.data["Description"] = std::move(description);
        collectObjectData(id, rel, reader, properties, element.data);

        elementGuids.emplace_back(id, element.guid);
        std::string key = element.guid;
        scene.elements.emplace(std::move(key), std::move(element));
    }

    // Spatial tree: every spatial structure element, linked through IfcRelAggregates.
    std::vector<uint32_t> spatialIds;
    for (uint32_t type : kSpatialTypes) {
        const auto ids = loader->GetExpressIDsWithType(type);
        spatialIds.insert(spatialIds.end(), ids.begin(), ids.end());
    }
    std::sort(spatialIds.begin(), spatialIds.end());

    std::unordered_map<uint32_t, std::string> spatialGuids;
    for (uint32_t id : spatialIds) {
        const uint32_t type = reader.lineType(id);
        IfcSpatialNode node;
        node.guid = uniqueGuid(reader.string(id, 0), id, scene.spatial);
        node.type = schemaManager.IfcTypeCodeToType(type);
        node.name = reader.string(id, 2);
        // LongName is attribute 5 on IfcProject and 7 on spatial elements.
        node.longName = reader.string(id, type == schema::IFCPROJECT ? 5 : 7);
        node.data["ExpressID"] = std::to_string(id);
        if (std::string description = reader.string(id, 3); !description.empty())
            node.data["Description"] = std::move(description);
        if (type == schema::IFCBUILDINGSTOREY)
            if (std::string elevation = reader.value(id, 9); !elevation.empty())
                node.data["Elevation"] = std::move(elevation);
        collectObjectData(id, rel, reader, properties, node.data);

        spatialGuids[id] = node.guid;
        std::string key = node.guid;
        scene.spatial.emplace(std::move(key), std::move(node));
    }

    for (uint32_t id : spatialIds) {
        const std::string& guid = spatialGuids[id];
        auto parentIt = rel.aggregateParent.find(id);
        auto parentGuid = parentIt != rel.aggregateParent.end() ? spatialGuids.find(parentIt->second) : spatialGuids.end();
        if (parentGuid == spatialGuids.end()) {
            scene.roots.push_back(guid);
            continue;
        }
        scene.spatial[guid].parentGuid = parentGuid->second;
        scene.spatial[parentGuid->second].childSpatialGuids.push_back(guid);
    }

    // An element's container is its own, or for parts (curtain wall panels, stair flights...)
    // the container of the nearest aggregating element that has one.
    auto findContainer = [&](uint32_t id) -> const std::string* {
        uint32_t current = id;
        for (int depth = 0; depth < kMaxNesting * 2; ++depth) {
            if (auto it = rel.container.find(current); it != rel.container.end()) {
                auto guid = spatialGuids.find(it->second);
                return guid != spatialGuids.end() ? &guid->second : nullptr;
            }
            auto parent = rel.aggregateParent.find(current);
            if (parent == rel.aggregateParent.end())
                return nullptr;
            current = parent->second;
            if (auto guid = spatialGuids.find(current); guid != spatialGuids.end())
                return &guid->second;
        }
        return nullptr;
    };

    auto findStorey = [&](std::string spatialGuid) -> std::string {
        for (int depth = 0; depth < kMaxNesting * 2 && !spatialGuid.empty(); ++depth) {
            const auto it = scene.spatial.find(spatialGuid);
            if (it == scene.spatial.end())
                break;
            if (it->second.type == "IfcBuildingStorey")
                return it->second.name;
            spatialGuid = it->second.parentGuid;
        }
        return {};
    };

    std::vector<std::string> orphans;
    for (const auto& [id, guid] : elementGuids) {
        IfcElement& element = scene.elements[guid];
        if (const std::string* container = findContainer(id)) {
            element.parentSpatialGuid = *container;
            element.storey = findStorey(*container);
            scene.spatial[*container].elementGuids.push_back(guid);
        }
        else {
            orphans.push_back(guid);
        }
    }

    // Elements with no spatial container still need to be reachable from the hierarchy.
    if (!orphans.empty()) {
        IfcSpatialNode node;
        node.guid = uniqueGuid("unassigned", 0, scene.spatial);
        node.type = "Unassigned";
        node.name = "Unassigned elements";
        for (const auto& guid : orphans)
            scene.elements[guid].parentSpatialGuid = node.guid;
        node.elementGuids = std::move(orphans);
        scene.roots.push_back(node.guid);
        std::string key = node.guid;
        scene.spatial.emplace(std::move(key), std::move(node));
    }

    scene.rebuildSubmeshMaps(result.submeshes.size());

    LOG_INFO("[IFC direct] " << scene.schema << ": " << result.vertices.size() << " verts, "
        << result.submeshes.size() << " submeshes, " << scene.elements.size() << " elements, "
        << scene.spatial.size() << " spatial nodes"
        << (failed ? ", " + std::to_string(failed) + " elements failed" : std::string()) << "\n");

    result.ifcScene = std::move(scene);
    return result;
}

} // namespace

Mesh loadIfcDirect(const std::string& ifcPath)
{
    std::ifstream file(ifcPath, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot open IFC file: " + ifcPath);

    std::lock_guard lock(g_webIfcMutex);
    Mesh result;
    runWithLargeStack([&] { result = importIfc(file, ifcPath); });
    return result;
}
