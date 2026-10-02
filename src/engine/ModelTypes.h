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

struct SubmeshInfo {
    uint32_t indexOffset;
    uint32_t indexCount;
    uint32_t vertexOffset;
    Material material;

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

    std::optional<IfcScene> ifcScene;
};