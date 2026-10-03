#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>
#include <optional>
#include "IfcScene.h"

// class IfcScene;

enum class AlphaMode : int {
    OPAQUE = 0,
    MASK = 1,
    BLEND = 2
};

struct Material {
    glm::vec4 baseColorFactor{ 1.0f };
    float metallicFactor{ 1.0f };
    float roughnessFactor{ 1.0f };
    int baseColorTextureIndex = -1;
    int normalTextureIndex = -1;
    int metallicRoughnessTextureIndex = -1;
    float alphaCutoff{ 0.5f };
    AlphaMode alphaMode{ AlphaMode::OPAQUE };
};

// An instance's replacement for a material's factors. Textures and alpha mode stay the model's.
struct MaterialOverride {
    glm::vec3 baseColor{ 1.0f };
    float metallic = 0.0f;
    float roughness = 0.5f;
};

struct SubmeshInfo {
    uint32_t indexOffset;
    uint32_t indexCount;
    uint32_t vertexOffset;
    Material material;
    // Index into Mesh/GPUModel::materialNames; -1 = the source gave this submesh no material.
    int materialSlot = -1;

    glm::vec3 boundsMin{ std::numeric_limits<float>::max() };
    glm::vec3 boundsMax{ std::numeric_limits<float>::lowest() };
};

struct TextureData {
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = nullptr;
    std::string path;
    const unsigned char* encodedData = nullptr;
    size_t encodedSize = 0;
    bool fromCache = false;
    bool isLinear = false;

    void free();
};

struct Vertex; // Forward declare, defined in Buffers.h

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<SubmeshInfo> submeshes;
    std::vector<TextureData> textureData;
    // One entry per material slot referenced by SubmeshInfo::materialSlot.
    std::vector<std::string> materialNames;

    std::optional<IfcScene> ifcScene;
};