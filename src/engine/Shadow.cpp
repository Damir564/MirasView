#include "Shadow.h"
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {

constexpr float kSplitLambda = 0.75f;   // blend of logarithmic (1) and uniform (0) split distances
constexpr float kCascadeOverlap = 0.1f; // must match the hand-over band in triangle.frag

glm::mat4 lightSpaceMatrix(const glm::vec3& lightDir, const glm::vec3& center, float radius, uint32_t mapSize,
    float& depthRange)
{
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (glm::abs(glm::dot(lightDir, up)) > 0.99f)
        up = glm::vec3(0.0f, 0.0f, 1.0f);

    // Rotation only, so snapping in this space is independent of where the camera is.
    const glm::mat4 lightView = glm::lookAt(glm::vec3(0.0f), lightDir, up);
    glm::vec3 centerLS = glm::vec3(lightView * glm::vec4(center, 1.0f));
    const float texel = (2.0f * radius) / static_cast<float>(mapSize);
    centerLS.x = std::floor(centerLS.x / texel) * texel;
    centerLS.y = std::floor(centerLS.y / texel) * texel;

    // Depth moves in coarse steps too, so a cascade the camera barely moved in keeps the same matrix
    // and its cached contents stay valid. View space looks down -Z.
    const float depthStep = radius * 0.25f;
    const float depth = std::floor(-centerLS.z / depthStep) * depthStep;
    // Casters between the sun and the near plane are not clipped: depth clamp flattens them onto it.
    const float nearPlane = depth - radius - depthStep;
    const float farPlane = depth + radius + depthStep;
    depthRange = farPlane - nearPlane;

    glm::mat4 lightProj = glm::orthoRH_ZO(centerLS.x - radius, centerLS.x + radius,
        centerLS.y - radius, centerLS.y + radius, nearPlane, farPlane);
    lightProj[1][1] *= -1; // Vulkan clip space has Y pointing down
    return lightProj * lightView;
}

} // namespace

void computeShadowCascades(const glm::mat4& view, const glm::mat4& proj, float nearPlane, float distance,
    const glm::vec3& lightDirection, uint32_t mapSize, std::span<ShadowCascade> cascades)
{
    if (cascades.empty())
        return;
    const glm::mat4 invView = glm::inverse(view);
    const glm::vec3 cameraPos(invView[3]);
    const glm::vec3 forward = -glm::normalize(glm::vec3(invView[2]));
    // Squared tangent of half the diagonal field of view.
    const float tanX = 1.0f / proj[0][0];
    const float tanY = 1.0f / std::abs(proj[1][1]);
    const float k = tanX * tanX + tanY * tanY;
    const glm::vec3 lightDir = glm::normalize(lightDirection);
    const float count = static_cast<float>(cascades.size());
    nearPlane = std::max(nearPlane, 0.01f);
    distance = std::max(distance, nearPlane * 2.0f);

    float previousStart = 0.0f;
    float previousEnd = 0.0f;
    for (size_t c = 0; c < cascades.size(); ++c) {
        const float s = static_cast<float>(c + 1) / count;
        const float logSplit = nearPlane * std::pow(distance / nearPlane, s);
        const float uniformSplit = nearPlane + (distance - nearPlane) * s;
        const float end = glm::mix(uniformSplit, logSplit, kSplitLambda);
        // Starts early enough to cover the band where the previous cascade hands over to this one.
        const float start = c == 0 ? nearPlane : previousEnd - (previousEnd - previousStart) * kCascadeOverlap;

        // Smallest sphere through the corners of the slice [start, end].
        float z = 0.5f * (start + end) * (1.0f + k);
        float radius = 0.0f;
        if (z >= end) {
            z = end;
            radius = end * std::sqrt(k);
        }
        else {
            radius = std::sqrt((end - z) * (end - z) + end * end * k);
        }

        ShadowCascade& cascade = cascades[c];
        cascade.splitEnd = end;
        cascade.radius = radius;
        cascade.texelWorld = 2.0f * radius / static_cast<float>(mapSize);
        cascade.matrix = lightSpaceMatrix(lightDir, cameraPos + forward * z, radius, mapSize, cascade.depthRange);

        previousStart = c == 0 ? 0.0f : previousEnd;
        previousEnd = end;
    }
}

ShadowMapResources createShadowMap(VmaAllocator allocator, vk::Device device, uint32_t size, uint32_t layers)
{
    ShadowMapResources shadow{};
    shadow.size = size;
    shadow.layers = std::clamp(layers, 1u, kMaxShadowCascades);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.extent = { size, size, 1 };
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = shadow.layers;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(allocator, &imageInfo, &allocInfo, &shadow.image, &shadow.allocation, nullptr) != VK_SUCCESS)
        throw std::runtime_error("Failed to create shadow map image");

    auto createView = [&](vk::ImageViewType type, uint32_t baseLayer, uint32_t layerCount) {
        vk::ImageViewCreateInfo viewInfo{};
        viewInfo.image = vk::Image(shadow.image);
        viewInfo.viewType = type;
        viewInfo.format = vk::Format::eD32Sfloat;
        viewInfo.subresourceRange = { vk::ImageAspectFlagBits::eDepth, 0, 1, baseLayer, layerCount };
        auto view = device.createImageView(viewInfo);
        if (view.result != vk::Result::eSuccess) {
            destroyShadowMap(shadow, allocator, device);
            throw std::runtime_error("Failed to create shadow map view");
        }
        return view.value;
    };
    shadow.view = createView(vk::ImageViewType::e2DArray, 0, shadow.layers);
    for (uint32_t layer = 0; layer < shadow.layers; ++layer)
        shadow.layerViews[layer] = createView(vk::ImageViewType::e2D, layer, 1);

    // Outside the map counts as lit: the border is the far plane.
    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eLinear;
    samplerInfo.minFilter = vk::Filter::eLinear;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eClampToBorder;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eClampToBorder;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eClampToBorder;
    samplerInfo.borderColor = vk::BorderColor::eFloatOpaqueWhite;
    samplerInfo.compareEnable = VK_TRUE;
    samplerInfo.compareOp = vk::CompareOp::eLessOrEqual;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    auto sampler = device.createSampler(samplerInfo);
    if (sampler.result != vk::Result::eSuccess) {
        destroyShadowMap(shadow, allocator, device);
        throw std::runtime_error("Failed to create shadow map sampler");
    }
    shadow.sampler = sampler.value;

    samplerInfo.magFilter = vk::Filter::eNearest;
    samplerInfo.minFilter = vk::Filter::eNearest;
    samplerInfo.compareEnable = VK_FALSE;
    auto depthSampler = device.createSampler(samplerInfo);
    if (depthSampler.result != vk::Result::eSuccess) {
        destroyShadowMap(shadow, allocator, device);
        throw std::runtime_error("Failed to create shadow depth sampler");
    }
    shadow.depthSampler = depthSampler.value;
    return shadow;
}

void destroyShadowMap(ShadowMapResources& shadow, VmaAllocator allocator, vk::Device device)
{
    if (shadow.sampler) device.destroySampler(shadow.sampler);
    if (shadow.depthSampler) device.destroySampler(shadow.depthSampler);
    for (vk::ImageView view : shadow.layerViews)
        if (view) device.destroyImageView(view);
    if (shadow.view) device.destroyImageView(shadow.view);
    if (shadow.image) vmaDestroyImage(allocator, shadow.image, shadow.allocation);
    shadow = {};
}
