#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cstddef>
#include <limits>
#include <algorithm>
#include <glm/ext/vector_float3.hpp>

struct Annotation {
    std::string text;
    std::string ifcGuid;
    int instanceIndex;
    glm::vec3 worldPosition; // where to render the label
};

struct IfcTypeInfo {
    std::string guid;
    std::string type;
    std::string name;
    std::string predefinedType;
};

struct IfcElement {
    std::string guid;
    std::string type;
    std::string name;
    std::string objectType;
    std::string tag;
    std::string storey;
    std::string parentSpatialGuid;

    std::optional<IfcTypeInfo> typeInfo;

    std::unordered_map<std::string, std::string> data;

    // An element can span several submeshes (one per material/colour).
    std::vector<std::size_t> submeshIndices;
    bool visible = true;
    bool selected = false;
};

struct IfcSpatialNode {
    std::string guid;
    std::string type;
    std::string name;
    std::string longName;
    std::string parentGuid;

    std::unordered_map<std::string, std::string> data;

    std::vector<std::string> childSpatialGuids;
    std::vector<std::string> elementGuids;

    bool visible = true;
    bool expanded = true;
    bool selected = false;
};

struct IfcScene {
    std::string schema;

    std::unordered_map<std::string, IfcElement> elements;
    std::unordered_map<std::string, IfcSpatialNode> spatial;
    std::vector<std::string> roots;

    std::unordered_map<std::size_t, std::string> submeshToGuid;

    std::vector<bool> submeshVisibilityCache;

    // Rebuilds submeshToGuid and the visibility cache from the elements' submeshIndices.
    void rebuildSubmeshMaps(std::size_t submeshCount) {
        submeshToGuid.clear();
        submeshVisibilityCache.assign(submeshCount, false);
        for (const auto& [guid, element] : elements) {
            for (std::size_t si : element.submeshIndices) {
                if (si >= submeshCount) continue;
                submeshToGuid[si] = guid;
                submeshVisibilityCache[si] = element.visible;
            }
        }
    }

    const std::vector<std::size_t>& submeshesOf(const std::string& guid) const {
        static const std::vector<std::size_t> kNone;
        auto it = elements.find(guid);
        return it != elements.end() ? it->second.submeshIndices : kNone;
    }

    bool isSubmeshVisible(std::size_t submeshIdx) const {
        if (submeshIdx < submeshVisibilityCache.size()) {
            return submeshVisibilityCache[submeshIdx];
        }
        return false;
    }

    void syncVisibilityCache() {
        for (const auto& [submeshIdx, guid] : submeshToGuid) {
            auto eit = elements.find(guid);
            if (eit != elements.end()) {
                if (submeshIdx < submeshVisibilityCache.size()) {
                    submeshVisibilityCache[submeshIdx] = eit->second.visible;
                }
            }
        }
    }

    //bool isSubmeshVisible(std::size_t submeshIdx) const {
    //    auto it = submeshToGuid.find(submeshIdx);
    //    if (it == submeshToGuid.end())
    //        return false;

    //    auto eit = elements.find(it->second);
    //    if (eit == elements.end())
    //        return false;

    //    return eit->second.visible;
    //}

    void setVisibilityRecursive(const std::string& spatialGuid, bool vis) {
        auto it = spatial.find(spatialGuid);
        if (it == spatial.end()) return;

        it->second.visible = vis;

        for (const auto& eg : it->second.elementGuids) {
            auto eit = elements.find(eg);
            if (eit != elements.end()) {
                eit->second.visible = vis;

                for (std::size_t si : eit->second.submeshIndices) {
                    if (si < submeshVisibilityCache.size())
                        submeshVisibilityCache[si] = vis;
                }
            }
        }

        for (auto& cg : it->second.childSpatialGuids)
            setVisibilityRecursive(cg, vis);
    }
};



