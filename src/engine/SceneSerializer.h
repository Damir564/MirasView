#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <map>
#include <glm/glm.hpp>

// Forward declarations - adjust these includes to match your project
#include "ModelManager.h" // For ModelManager, ModelInstance, GPUModel
#include "Log.h"

// Version 1 files have no per-instance color, version 2 no per-instance materials.
inline constexpr uint32_t kSceneFileVersion = 3;

struct SceneFileHeader {
    char magic[4] = { 'S', 'C', 'N', 'E' };
    uint32_t version = kSceneFileVersion;
    uint32_t modelCount = 0;
    uint32_t instanceCount = 0;
};

struct SceneModelEntry {
    uint32_t pathLength = 0;
    uint32_t nameLength = 0;
    // followed by: char path[pathLength], char name[nameLength]
};

struct SceneInstanceEntry {
    uint32_t modelIndex = 0; // index into the model list in this file
    uint32_t nameLength = 0;
    float posX, posY, posZ;
    float rotX, rotY, rotZ;
    float scaleX, scaleY, scaleZ;
    bool visible = true;
    // followed by (version 2+): float color[3];
    // (version 3+): uint32_t materialCount, then per material: int32_t slot, float baseColor[3], metallic, roughness;
    // then: char name[nameLength]
};

class SceneSerializer {
public:
    static bool Save(const std::string& filepath, ModelManager& modelManager) {
        std::ofstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            LOG_ERROR("[SCENE] Failed to open file for writing: " << filepath << "\n");
            return false;
        }

        const auto& models = modelManager.getModels();
        const auto& instances = modelManager.getInstances();

        // Build unique model path list (deduplicate)
        struct ModelEntry {
            std::string path;
            std::string name;
        };
        std::vector<ModelEntry> uniqueModels;
        // Map from modelManager model index -> file model index
        std::vector<uint32_t> modelIndexMap(models.size(), 0);

        for (size_t i = 0; i < models.size(); ++i) {
            if (!models[i] || !models[i]->isValid()) continue;

            bool found = false;
            for (size_t j = 0; j < uniqueModels.size(); ++j) {
                if (uniqueModels[j].path == models[i]->sourcePath) {
                    modelIndexMap[i] = static_cast<uint32_t>(j);
                    found = true;
                    break;
                }
            }
            if (!found) {
                modelIndexMap[i] = static_cast<uint32_t>(uniqueModels.size());
                uniqueModels.push_back({ models[i]->sourcePath, models[i]->name });
            }
        }

        // Count valid instances
        uint32_t validInstanceCount = 0;
        for (const auto& inst : instances) {
            if (inst.modelIndex < models.size() && models[inst.modelIndex] && models[inst.modelIndex]->isValid()) {
                validInstanceCount++;
            }
        }

        // Write header
        SceneFileHeader header;
        header.modelCount = static_cast<uint32_t>(uniqueModels.size());
        header.instanceCount = validInstanceCount;
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));

        // Write models
        for (const auto& model : uniqueModels) {
            SceneModelEntry entry;
            entry.pathLength = static_cast<uint32_t>(model.path.size());
            entry.nameLength = static_cast<uint32_t>(model.name.size());
            file.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
            file.write(model.path.data(), entry.pathLength);
            file.write(model.name.data(), entry.nameLength);
        }

        // Write instances
        for (const auto& inst : instances) {
            if (inst.modelIndex >= models.size() || !models[inst.modelIndex] || !models[inst.modelIndex]->isValid()) {
                continue;
            }

            SceneInstanceEntry entry;
            entry.modelIndex = modelIndexMap[inst.modelIndex];
            entry.nameLength = static_cast<uint32_t>(inst.name.size());
            entry.posX = inst.position.x;
            entry.posY = inst.position.y;
            entry.posZ = inst.position.z;
            entry.rotX = inst.rotation.x;
            entry.rotY = inst.rotation.y;
            entry.rotZ = inst.rotation.z;
            entry.scaleX = inst.scale.x;
            entry.scaleY = inst.scale.y;
            entry.scaleZ = inst.scale.z;
            entry.visible = inst.visible;

            file.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
            const float color[3] = { inst.color.r, inst.color.g, inst.color.b };
            file.write(reinterpret_cast<const char*>(color), sizeof(color));
            const uint32_t materialCount = static_cast<uint32_t>(inst.materials.size());
            file.write(reinterpret_cast<const char*>(&materialCount), sizeof(materialCount));
            for (const auto& [slot, material] : inst.materials) {
                const int32_t slotValue = slot;
                const float values[5] = { material.baseColor.r, material.baseColor.g, material.baseColor.b,
                    material.metallic, material.roughness };
                file.write(reinterpret_cast<const char*>(&slotValue), sizeof(slotValue));
                file.write(reinterpret_cast<const char*>(values), sizeof(values));
            }
            file.write(inst.name.data(), entry.nameLength);
        }

        file.close();
        LOG_INFO("[SCENE] Saved: " << uniqueModels.size() << " models, "
            << validInstanceCount << " instances to " << filepath << "\n");
        return true;
    }

    struct LoadedScene {
        struct LoadedModel {
            std::string path;
            std::string name;
        };
        struct LoadedInstance {
            uint32_t fileModelIndex; // index into loadedModels
            std::string name;
            glm::vec3 position;
            glm::vec3 rotation;
            glm::vec3 scale;
            bool visible;
            glm::vec3 color{ 1.0f };
            std::map<int, MaterialOverride> materials;
        };

        std::vector<LoadedModel> models;
        std::vector<LoadedInstance> instances;
        bool valid = false;
    };

    static LoadedScene Load(const std::string& filepath) {
        LoadedScene scene;
        scene.valid = false;

        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            LOG_ERROR("[SCENE] Failed to open file for reading: " << filepath << "\n");
            return scene;
        }

        // Read header
        SceneFileHeader header;
        file.read(reinterpret_cast<char*>(&header), sizeof(header));

        if (header.magic[0] != 'S' || header.magic[1] != 'C' ||
            header.magic[2] != 'N' || header.magic[3] != 'E') {
            LOG_ERROR("[SCENE] Invalid scene file magic\n");
            return scene;
        }

        if (header.version < 1 || header.version > kSceneFileVersion) {
            LOG_ERROR("[SCENE] Unsupported scene version: " << header.version << "\n");
            return scene;
        }

        // Read models
        scene.models.resize(header.modelCount);
        for (uint32_t i = 0; i < header.modelCount; ++i) {
            SceneModelEntry entry;
            file.read(reinterpret_cast<char*>(&entry), sizeof(entry));

            scene.models[i].path.resize(entry.pathLength);
            file.read(scene.models[i].path.data(), entry.pathLength);

            scene.models[i].name.resize(entry.nameLength);
            file.read(scene.models[i].name.data(), entry.nameLength);

            // Older scenes marked web-ifc imports with this prefix; web-ifc is now the only IFC importer.
            constexpr std::string_view kLegacyIfcPrefix = "ifcdirect:";
            if (scene.models[i].path.starts_with(kLegacyIfcPrefix))
                scene.models[i].path.erase(0, kLegacyIfcPrefix.size());
        }

        // Read instances
        scene.instances.resize(header.instanceCount);
        for (uint32_t i = 0; i < header.instanceCount; ++i) {
            SceneInstanceEntry entry;
            file.read(reinterpret_cast<char*>(&entry), sizeof(entry));

            scene.instances[i].fileModelIndex = entry.modelIndex;
            scene.instances[i].position = glm::vec3(entry.posX, entry.posY, entry.posZ);
            scene.instances[i].rotation = glm::vec3(entry.rotX, entry.rotY, entry.rotZ);
            scene.instances[i].scale = glm::vec3(entry.scaleX, entry.scaleY, entry.scaleZ);
            scene.instances[i].visible = entry.visible;
            if (header.version >= 2) {
                float color[3] = { 1.0f, 1.0f, 1.0f };
                file.read(reinterpret_cast<char*>(color), sizeof(color));
                scene.instances[i].color = glm::vec3(color[0], color[1], color[2]);
            }
            if (header.version >= 3) {
                uint32_t materialCount = 0;
                file.read(reinterpret_cast<char*>(&materialCount), sizeof(materialCount));
                // A corrupt count must not turn into a near-endless read loop.
                if (!file || materialCount > (1u << 20)) {
                    LOG_ERROR("[SCENE] Scene file is corrupt: " << filepath << "\n");
                    return scene;
                }
                for (uint32_t m = 0; m < materialCount; ++m) {
                    int32_t slot = -1;
                    float values[5] = {};
                    file.read(reinterpret_cast<char*>(&slot), sizeof(slot));
                    file.read(reinterpret_cast<char*>(values), sizeof(values));
                    scene.instances[i].materials[slot] =
                        MaterialOverride{ glm::vec3(values[0], values[1], values[2]), values[3], values[4] };
                }
            }

            scene.instances[i].name.resize(entry.nameLength);
            file.read(scene.instances[i].name.data(), entry.nameLength);
        }

        if (!file) {
            LOG_ERROR("[SCENE] Scene file is truncated: " << filepath << "\n");
            return scene;
        }
        scene.valid = true;
        LOG_INFO("[SCENE] Loaded: " << scene.models.size() << " models, "
            << scene.instances.size() << " instances from " << filepath << "\n");
        return scene;
    }
};