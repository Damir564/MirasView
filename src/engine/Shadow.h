#pragma once
#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <span>

inline constexpr uint32_t kMaxShadowCascades = 4;

// A depth array with one layer per cascade.
struct ShadowMapResources {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    vk::ImageView view;                                         // all layers, for sampling
    std::array<vk::ImageView, kMaxShadowCascades> layerViews{}; // one layer each, for rendering
    vk::Sampler sampler;      // depth comparison (hardware PCF)
    vk::Sampler depthSampler; // raw depth, for the soft-shadow blocker search
    uint32_t size = 0;
    uint32_t layers = 0;
};

struct ShadowCascade {
    glm::mat4 matrix{ 1.0f }; // world -> shadow clip space
    float splitEnd = 0.0f;    // view depth where the cascade ends
    float radius = 1.0f;      // radius of the sphere the cascade covers
    float texelWorld = 0.0f;  // world size of one shadow-map texel
    float depthRange = 1.0f;  // world distance covered by shadow depth 0..1
};

// Splits the view frustum up to `distance` into cascades. Each one covers a sphere around its slice of
// the frustum, so its size (and texel size) stays constant while the camera turns, and the sphere center is
// snapped to whole texels so shadow edges do not shimmer while the camera moves.
void computeShadowCascades(const glm::mat4& view, const glm::mat4& proj, float nearPlane, float distance,
    const glm::vec3& lightDirection, uint32_t mapSize, std::span<ShadowCascade> cascades);

// Throws std::runtime_error on failure.
ShadowMapResources createShadowMap(VmaAllocator allocator, vk::Device device, uint32_t size, uint32_t layers);
void destroyShadowMap(ShadowMapResources& shadow, VmaAllocator allocator, vk::Device device);
