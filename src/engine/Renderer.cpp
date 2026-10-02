#include "Renderer.h"
#include "Atmosphere.h"
#include "ModelManager.h"
#include "RenderUtils.h"
#include "Vertex.h"
#include "VulkanContext.h"
#include <volk.h>
#include <SDL3/SDL.h>
#include <glm/ext/matrix_transform.hpp>
#include "imgui.h"
#include "backends/imgui_impl_vulkan.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <execution>
#include <numeric>
#include "Log.h"

namespace {

constexpr uint8_t kVisibleMain = 1;
// A submesh casts into cascade c when bit (kVisibleShadow << c) is set.
constexpr uint8_t kVisibleShadow = 2;
constexpr size_t kCullChunkSize = 1024;
// Unity's selection orange; occluded parts of the outline are drawn fainter.
constexpr glm::vec3 kOutlineColor{ 1.0f, 0.4f, 0.0f };
constexpr float kOutlineOccludedAlpha = 0.4f;
constexpr float kOutlineWidthPixels = 2.0f;

// Sun irradiance above the atmosphere at intensity 1, in the units exposure 0 is calibrated for.
constexpr float kSunIrradiance = 3.2f;
constexpr float kGroundAlbedo = 0.3f;
// Height fog at fogDensity 1: a few kilometers of visibility near the ground, thinning out with height.
constexpr float kFogDensity = 0.00015f;
constexpr float kFogHeightFalloff = 1.0f / 400.0f;
// Width of a soft shadow's penumbra per meter between caster and receiver: the sun's disk, widened a
// little for the scattering of the atmosphere.
constexpr float kPenumbraPerMeter = 2.0f * kSunAngularRadius * 1.5f;
// SSAO samples for the low / medium / high settings.
constexpr int kAoSamples[] = { 0, 8, 12, 20 };

const vk::VertexInputBindingDescription2EXT kLineBinding{ 0, sizeof(GizmoVertex), vk::VertexInputRate::eVertex, 1 };
const std::array<vk::VertexInputAttributeDescription2EXT, 2> kLineAttributes = { {
    { 0, 0, vk::Format::eR32G32B32Sfloat, static_cast<uint32_t>(offsetof(GizmoVertex, position)) },
    { 1, 0, vk::Format::eR32G32B32Sfloat, static_cast<uint32_t>(offsetof(GizmoVertex, color)) },
} };

// FNV-1a, used to detect frames where a shadow cascade or the sky would come out identical.
class Hasher {
public:
    void bytes(const void* data, size_t size) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i)
            byte(p[i]);
    }
    void byte(uint8_t value) {
        m_hash ^= value;
        m_hash *= 1099511628211ull;
    }
    uint64_t value() const { return m_hash; }

private:
    uint64_t m_hash = 1469598103934665603ull;
};

vk::SampleCountFlagBits toSampleCount(int samples)
{
    switch (samples) {
    case 8: return vk::SampleCountFlagBits::e8;
    case 4: return vk::SampleCountFlagBits::e4;
    case 2: return vk::SampleCountFlagBits::e2;
    default: return vk::SampleCountFlagBits::e1;
    }
}

// Perspective near/far recovered from a right-handed zero-to-one projection matrix.
glm::vec2 projectionDepthRange(const glm::mat4& proj)
{
    const float a = proj[2][2];
    const float b = proj[3][2];
    if (std::abs(a) < 1e-12f || std::abs(a + 1.0f) < 1e-12f)
        return { 0.1f, 1000.0f };
    return { b / a, b / (a + 1.0f) };
}

} // namespace

Renderer::~Renderer()
{
    shutdown();
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool Renderer::init(VulkanContext& context, SDL_Window* window, const GraphicsSettings& settings)
{
    m_context = &context;
    m_window = window;
    m_device = context.device();
    m_allocator = context.allocator();
    m_meshBinding = Vertex::getBindingDescription(0);
    m_meshAttributes = Vertex::getVertexOnlyAttributes(0);
    m_settings = sanitizeGraphicsSettings(settings);
    updateSun();
    queryCapabilities();

    try {
        int pixelWidth = 0, pixelHeight = 0;
        SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
        if (!m_swapchain.create(context, static_cast<uint32_t>(pixelWidth), static_cast<uint32_t>(pixelHeight), m_settings.vsync)) {
            shutdown();
            return false;
        }
        // Fixed at startup; later swapchain rebuilds may change the image count but not this.
        m_framesInFlight = m_swapchain.imageCount();

        if (!createRenderTargets() || !createSkyResources() || !createFrameResources() || !createDescriptors() ||
            !createShaders() || !initImGuiBackend()) {
            shutdown();
            return false;
        }
    }
    catch (const std::exception& e) {
        LOG_ERROR("Renderer initialization failed: " << e.what() << "\n");
        shutdown();
        return false;
    }
    return true;
}

void Renderer::queryCapabilities()
{
    const vk::PhysicalDeviceLimits limits = m_context->physicalDevice().getProperties().limits;
    const vk::SampleCountFlags counts = limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts;
    m_capabilities.maxMsaaSamples = 1;
    for (int samples : { 2, 4, 8 })
        if (counts & toSampleCount(samples))
            m_capabilities.maxMsaaSamples = samples;
    const OptionalDeviceFeatures& features = m_context->features();
    m_capabilities.maxAnisotropy = features.samplerAnisotropy ? limits.maxSamplerAnisotropy : 1.0f;
    m_maxDrawIndirectCount = features.multiDrawIndirect ? limits.maxDrawIndirectCount : 1;
}

vk::SampleCountFlagBits Renderer::effectiveSampleCount() const
{
    return toSampleCount(std::min(m_settings.msaaSamples, m_capabilities.maxMsaaSamples));
}

bool Renderer::createRenderImage(vk::Format format, vk::ImageUsageFlags usage, vk::SampleCountFlagBits samples,
    vk::ImageAspectFlags aspect, RenderImage& out, vk::Extent2D extent, uint32_t mipLevels)
{
    if (extent.width == 0 || extent.height == 0)
        extent = m_swapchain.extent();

    vk::ImageCreateInfo imageInfo{};
    imageInfo.imageType = vk::ImageType::e2D;
    imageInfo.extent = vk::Extent3D{ extent.width, extent.height, 1 };
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = vk::ImageTiling::eOptimal;
    imageInfo.initialLayout = vk::ImageLayout::eUndefined;
    imageInfo.usage = usage;
    imageInfo.samples = samples;
    imageInfo.sharingMode = vk::SharingMode::eExclusive;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (usage & vk::ImageUsageFlagBits::eTransientAttachment)
        allocInfo.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;

    if (vmaCreateImage(m_allocator, reinterpret_cast<const VkImageCreateInfo*>(&imageInfo), &allocInfo,
        &out.image, &out.allocation, nullptr) != VK_SUCCESS) {
        LOG_ERROR("Failed to create render target image\n");
        out = {};
        return false;
    }

    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = vk::Image(out.image);
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = { aspect, 0, mipLevels, 0, 1 };
    if (!takeResult(m_device.createImageView(viewInfo), out.view, "render target view")) {
        destroyRenderImage(out);
        return false;
    }
    return true;
}

bool Renderer::createMipView(const RenderImage& image, vk::Format format, uint32_t mip, vk::ImageView& out)
{
    vk::ImageViewCreateInfo viewInfo{};
    viewInfo.image = vk::Image(image.image);
    viewInfo.viewType = vk::ImageViewType::e2D;
    viewInfo.format = format;
    viewInfo.subresourceRange = { vk::ImageAspectFlagBits::eColor, mip, 1, 0, 1 };
    return takeResult(m_device.createImageView(viewInfo), out, "mip view");
}

void Renderer::destroyRenderImage(RenderImage& image)
{
    if (image.view) m_device.destroyImageView(image.view);
    if (image.image) vmaDestroyImage(m_allocator, image.image, image.allocation);
    image = {};
}

bool Renderer::createRenderTargets()
{
    m_samples = effectiveSampleCount();
    const vk::Extent2D extent = m_swapchain.extent();
    m_halfExtent.setWidth((extent.width + 1) / 2);
    m_halfExtent.setHeight((extent.height + 1) / 2);
    const auto depthAspect = vk::ImageAspectFlagBits::eDepth;
    const auto colorAspect = vk::ImageAspectFlagBits::eColor;
    const auto target = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
    const auto single = vk::SampleCountFlagBits::e1;

    if (!createRenderImage(kDepthFormat,
            vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled,
            single, depthAspect, m_sceneDepth) ||
        !createRenderImage(kSelectionMaskFormat, target, single, colorAspect, m_selectionMask) ||
        !createRenderImage(kHdrFormat, target, single, colorAspect, m_hdrColor) ||
        !createRenderImage(kLdrFormat, target, single, colorAspect, m_ldrColor) ||
        !createRenderImage(kAoDepthFormat, target, single, colorAspect, m_aoDepth, m_halfExtent) ||
        !createRenderImage(kAoFormat, target, single, colorAspect, m_aoRaw, m_halfExtent) ||
        !createRenderImage(kAoFormat, target, single, colorAspect, m_aoTemp, m_halfExtent))
        return false;

    // Bloom mips down to roughly 8 texels on the short side.
    m_bloomMips = 1;
    while (m_bloomMips < kMaxBloomMips && (std::min(m_halfExtent.width, m_halfExtent.height) >> m_bloomMips) >= 8)
        ++m_bloomMips;
    if (!createRenderImage(kHdrFormat, target, single, colorAspect, m_bloom, m_halfExtent, m_bloomMips))
        return false;
    for (uint32_t mip = 0; mip < m_bloomMips; ++mip)
        if (!createMipView(m_bloom, kHdrFormat, mip, m_bloomMipViews[mip]))
            return false;

    if (m_samples != single) {
        // The depth is not transient: the depth prepass result is loaded again by the scene pass.
        if (!createRenderImage(kHdrFormat,
                vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransientAttachment,
                m_samples, colorAspect, m_msaaColor) ||
            !createRenderImage(kDepthFormat, vk::ImageUsageFlagBits::eDepthStencilAttachment,
                m_samples, depthAspect, m_msaaDepth))
            return false;
    }

    writeImageDescriptors();
    return true;
}

void Renderer::destroyRenderTargets()
{
    for (vk::ImageView& view : m_bloomMipViews) {
        if (view) m_device.destroyImageView(view);
        view = nullptr;
    }
    m_bloomMips = 0;
    for (RenderImage* image : { &m_sceneDepth, &m_selectionMask, &m_msaaColor, &m_msaaDepth, &m_hdrColor,
             &m_ldrColor, &m_aoDepth, &m_aoRaw, &m_aoTemp, &m_bloom })
        destroyRenderImage(*image);
}

bool Renderer::createSkyResources()
{
    const auto colorAspect = vk::ImageAspectFlagBits::eColor;
    // Mips come from blits and give rough reflections their blur.
    const auto lutUsage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled |
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
    m_skyValid = false;
    return createRenderImage(kHdrFormat, lutUsage, vk::SampleCountFlagBits::e1, colorAspect, m_skyLut,
               kSkyLutExtent, kSkyLutMips) &&
        createMipView(m_skyLut, kHdrFormat, 0, m_skyLutTargetView) &&
        createRenderImage(kHdrFormat, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
            vk::SampleCountFlagBits::e1, colorAspect, m_skyIrradiance, kSkyIrradianceExtent);
}

void Renderer::destroySkyResources()
{
    if (m_skyLutTargetView) m_device.destroyImageView(m_skyLutTargetView);
    m_skyLutTargetView = nullptr;
    destroyRenderImage(m_skyLut);
    destroyRenderImage(m_skyIrradiance);
}

bool Renderer::createShadowMapResources()
{
    m_shadowMap = createShadowMap(m_allocator, m_device, static_cast<uint32_t>(m_settings.shadowMapSize),
        static_cast<uint32_t>(m_settings.shadowCascades));
    m_shadowMapValid = false;
    writeImageDescriptors();
    return true;
}

bool Renderer::createFrameResources()
{
    vk::CommandPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    poolInfo.queueFamilyIndex = m_context->graphicsQueueFamily();
    if (!takeResult(m_device.createCommandPool(poolInfo), m_commandPool, "command pool"))
        return false;

    vk::CommandBufferAllocateInfo allocInfo{};
    allocInfo.commandPool = m_commandPool;
    allocInfo.level = vk::CommandBufferLevel::ePrimary;
    allocInfo.commandBufferCount = m_framesInFlight;
    if (!takeResult(m_device.allocateCommandBuffers(allocInfo), m_commandBuffers, "command buffers"))
        return false;

    m_frameUBOs.resize(m_framesInFlight);
    for (UBOBuffer& ubo : m_frameUBOs) {
        VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bufferInfo.size = sizeof(FrameUBO);
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo uboAllocInfo{};
        uboAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        uboAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocationInfo{};
        if (vmaCreateBuffer(m_allocator, &bufferInfo, &uboAllocInfo, &ubo.buffer, &ubo.allocation, &allocationInfo) != VK_SUCCESS) {
            LOG_ERROR("Failed to create UBO buffer\n");
            ubo = {};
            return false;
        }
        ubo.mapped = allocationInfo.pMappedData;
    }

    vk::FenceCreateInfo fenceInfo{ vk::FenceCreateFlagBits::eSignaled };
    m_imageAvailableSemaphores.resize(m_framesInFlight);
    m_renderFinishedSemaphores.resize(m_framesInFlight);
    m_inFlightFences.resize(m_framesInFlight);
    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        if (!takeResult(m_device.createSemaphore({}), m_imageAvailableSemaphores[i], "semaphore") ||
            !takeResult(m_device.createSemaphore({}), m_renderFinishedSemaphores[i], "semaphore") ||
            !takeResult(m_device.createFence(fenceInfo), m_inFlightFences[i], "fence"))
            return false;
    }
    return true;
}

bool Renderer::createDescriptors()
{
    vk::SamplerCreateInfo samplerInfo{};
    samplerInfo.magFilter = vk::Filter::eLinear;
    samplerInfo.minFilter = vk::Filter::eLinear;
    samplerInfo.addressModeU = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeV = vk::SamplerAddressMode::eRepeat;
    samplerInfo.addressModeW = vk::SamplerAddressMode::eRepeat;
    samplerInfo.anisotropyEnable = m_capabilities.maxAnisotropy > 1.0f ? VK_TRUE : VK_FALSE;
    samplerInfo.maxAnisotropy = std::min(16.0f, m_capabilities.maxAnisotropy);
    samplerInfo.borderColor = vk::BorderColor::eIntOpaqueBlack;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
    samplerInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (!takeResult(m_device.createSampler(samplerInfo), m_textureSampler, "texture sampler"))
        return false;

    vk::SamplerCreateInfo nearestInfo{};
    nearestInfo.magFilter = vk::Filter::eNearest;
    nearestInfo.minFilter = vk::Filter::eNearest;
    nearestInfo.mipmapMode = vk::SamplerMipmapMode::eNearest;
    nearestInfo.addressModeU = vk::SamplerAddressMode::eClampToEdge;
    nearestInfo.addressModeV = vk::SamplerAddressMode::eClampToEdge;
    nearestInfo.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    if (!takeResult(m_device.createSampler(nearestInfo), m_nearestSampler, "nearest sampler"))
        return false;

    vk::SamplerCreateInfo linearInfo = nearestInfo;
    linearInfo.magFilter = vk::Filter::eLinear;
    linearInfo.minFilter = vk::Filter::eLinear;
    if (!takeResult(m_device.createSampler(linearInfo), m_linearClampSampler, "linear sampler"))
        return false;

    vk::SamplerCreateInfo skyInfo = linearInfo;
    skyInfo.mipmapMode = vk::SamplerMipmapMode::eLinear;
    skyInfo.addressModeU = vk::SamplerAddressMode::eRepeat;
    skyInfo.maxLod = VK_LOD_CLAMP_NONE;
    if (!takeResult(m_device.createSampler(skyInfo), m_skySampler, "sky sampler"))
        return false;

    const vk::DescriptorSetLayoutBinding textureBinding{ 0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment };
    if (!takeResult(m_device.createDescriptorSetLayout({ {}, 1, &textureBinding }), m_textureSetLayout, "texture set layout"))
        return false;

    // Binding 0: FrameUBO, 1: per-draw data (GpuDrawData[]), 2: per-instance transforms (GpuTransform[]).
    const vk::DescriptorSetLayoutBinding frameBindings[3] = {
        { 0, vk::DescriptorType::eUniformBuffer, 1, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment },
        { 1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment },
        { 2, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eVertex },
    };
    if (!takeResult(m_device.createDescriptorSetLayout({ {}, 3, frameBindings }), m_frameSetLayout, "frame set layout"))
        return false;

    // Set 4 of triangle.frag: shadow map (comparison and raw depth), AO, AO depth, sky LUT, sky irradiance.
    std::array<vk::DescriptorSetLayoutBinding, 6> lightingBindings;
    for (uint32_t i = 0; i < lightingBindings.size(); ++i)
        lightingBindings[i] = { i, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eFragment };
    vk::DescriptorSetLayoutCreateInfo lightingInfo{};
    lightingInfo.setBindings(lightingBindings);
    if (!takeResult(m_device.createDescriptorSetLayout(lightingInfo), m_lightingSetLayout, "lighting set layout"))
        return false;

    // Shared with ModelManager, which allocates one set per texture from it.
    const vk::DescriptorPoolSize poolSizes[] = {
        { vk::DescriptorType::eCombinedImageSampler, 1100 },
        { vk::DescriptorType::eUniformBuffer, m_framesInFlight + 10 },
        { vk::DescriptorType::eStorageBuffer, 2 * m_framesInFlight + 10 },
    };
    vk::DescriptorPoolCreateInfo poolInfo{};
    poolInfo.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    poolInfo.maxSets = 1164;
    poolInfo.setPoolSizes(poolSizes);
    if (!takeResult(m_device.createDescriptorPool(poolInfo), m_descriptorPool, "descriptor pool"))
        return false;

    std::vector<vk::DescriptorSet> lightingSets;
    if (!takeResult(m_device.allocateDescriptorSets({ m_descriptorPool, 1, &m_lightingSetLayout }), lightingSets, "lighting set"))
        return false;
    m_lightingSet = lightingSets[0];

    vk::DescriptorSet* imageSets[] = { &m_sceneDepthSet, &m_selectionMaskSet, &m_hdrSet, &m_ldrSet, &m_aoDepthSet,
        &m_aoRawSet, &m_aoTempSet, &m_skyLutSet };
    const uint32_t imageSetCount = static_cast<uint32_t>(std::size(imageSets)) + kMaxBloomMips;
    const std::vector<vk::DescriptorSetLayout> imageLayouts(imageSetCount, m_textureSetLayout);
    std::vector<vk::DescriptorSet> allocated;
    if (!takeResult(m_device.allocateDescriptorSets({ m_descriptorPool, imageLayouts }), allocated, "image descriptor sets"))
        return false;
    for (size_t i = 0; i < std::size(imageSets); ++i)
        *imageSets[i] = allocated[i];
    for (uint32_t i = 0; i < kMaxBloomMips; ++i)
        m_bloomSets[i] = allocated[std::size(imageSets) + i];

    // Also writes every image descriptor now that the sets exist.
    if (!createShadowMapResources())
        return false;

    const std::vector<vk::DescriptorSetLayout> frameLayouts(m_framesInFlight, m_frameSetLayout);
    vk::DescriptorSetAllocateInfo frameAllocInfo{ m_descriptorPool, frameLayouts };
    if (!takeResult(m_device.allocateDescriptorSets(frameAllocInfo), m_frameSets, "frame descriptor sets"))
        return false;

    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        const vk::DescriptorBufferInfo bufferInfo{ vk::Buffer(m_frameUBOs[i].buffer), 0, sizeof(FrameUBO) };
        const vk::WriteDescriptorSet write{ m_frameSets[i], 0, 0, 1, vk::DescriptorType::eUniformBuffer, nullptr, &bufferInfo };
        m_device.updateDescriptorSets(1, &write, 0, nullptr);
    }

    for (uint32_t i = 0; i < m_framesInFlight; ++i) {
        m_frameDrawBuffers.emplace_back(new FrameDrawBuffers{
            HostBuffer(m_allocator, vk::BufferUsageFlagBits::eStorageBuffer, sizeof(GpuDrawData) * 4096),
            HostBuffer(m_allocator, vk::BufferUsageFlagBits::eStorageBuffer, sizeof(GpuTransform) * 256),
            HostBuffer(m_allocator, vk::BufferUsageFlagBits::eIndirectBuffer, sizeof(vk::DrawIndexedIndirectCommand) * 4096),
        });
        writeDrawDescriptors(i);
    }
    return true;
}

void Renderer::writeDrawDescriptors(uint32_t frame)
{
    const FrameDrawBuffers& buffers = *m_frameDrawBuffers[frame];
    const vk::DescriptorBufferInfo drawsInfo(buffers.draws.getBuffer(), 0, VK_WHOLE_SIZE);
    const vk::DescriptorBufferInfo transformsInfo(buffers.transforms.getBuffer(), 0, VK_WHOLE_SIZE);
    const vk::WriteDescriptorSet writes[2] = {
        vk::WriteDescriptorSet(m_frameSets[frame], 1, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &drawsInfo),
        vk::WriteDescriptorSet(m_frameSets[frame], 2, 0, 1, vk::DescriptorType::eStorageBuffer, nullptr, &transformsInfo),
    };
    m_device.updateDescriptorSets(2, writes, 0, nullptr);
}

void Renderer::writeImageDescriptors()
{
    // Before createDescriptors() ran there is nothing to write; it calls this itself once the sets exist.
    if (!m_lightingSet || !m_shadowMap.view)
        return;

    constexpr size_t kMaxWrites = 16 + kMaxBloomMips;
    std::array<vk::DescriptorImageInfo, kMaxWrites> infos;
    std::array<vk::WriteDescriptorSet, kMaxWrites> writes;
    uint32_t count = 0;
    auto add = [&](vk::DescriptorSet set, uint32_t binding, vk::Sampler sampler, vk::ImageView view, vk::ImageLayout layout) {
        infos[count] = { sampler, view, layout };
        writes[count] = { set, binding, 0, 1, vk::DescriptorType::eCombinedImageSampler, &infos[count] };
        ++count;
    };
    const auto sampled = vk::ImageLayout::eShaderReadOnlyOptimal;
    const auto shadowLayout = vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    add(m_lightingSet, 0, m_shadowMap.sampler, m_shadowMap.view, shadowLayout);
    add(m_lightingSet, 1, m_shadowMap.depthSampler, m_shadowMap.view, shadowLayout);
    add(m_lightingSet, 2, m_nearestSampler, m_aoRaw.view, sampled);
    add(m_lightingSet, 3, m_nearestSampler, m_aoDepth.view, sampled);
    add(m_lightingSet, 4, m_skySampler, m_skyLut.view, sampled);
    add(m_lightingSet, 5, m_skySampler, m_skyIrradiance.view, sampled);
    add(m_sceneDepthSet, 0, m_nearestSampler, m_sceneDepth.view, vk::ImageLayout::eDepthReadOnlyOptimal);
    add(m_selectionMaskSet, 0, m_nearestSampler, m_selectionMask.view, sampled);
    add(m_hdrSet, 0, m_linearClampSampler, m_hdrColor.view, sampled);
    add(m_ldrSet, 0, m_linearClampSampler, m_ldrColor.view, sampled);
    add(m_aoDepthSet, 0, m_nearestSampler, m_aoDepth.view, sampled);
    add(m_aoRawSet, 0, m_nearestSampler, m_aoRaw.view, sampled);
    add(m_aoTempSet, 0, m_nearestSampler, m_aoTemp.view, sampled);
    add(m_skyLutSet, 0, m_skySampler, m_skyLut.view, sampled);
    // Sets past the current chain length are never used; they just need a valid view.
    for (uint32_t i = 0; i < kMaxBloomMips; ++i)
        add(m_bloomSets[i], 0, m_linearClampSampler, m_bloomMipViews[std::min(i, m_bloomMips - 1)], sampled);
    m_device.updateDescriptorSets(count, writes.data(), 0, nullptr);
}

bool Renderer::createShaders()
{
    // Main: set 0 = frame data, sets 1-3 = base color / normal / metallic-roughness, set 4 = lighting.
    const vk::DescriptorSetLayout meshLayouts[] = {
        m_frameSetLayout, m_textureSetLayout, m_textureSetLayout, m_textureSetLayout, m_lightingSetLayout,
    };
    // Depth-only (shadow, prepass): set 0 = frame data, set 1 = base color for alpha testing.
    const vk::DescriptorSetLayout depthLayouts[] = { m_frameSetLayout, m_textureSetLayout };
    const vk::DescriptorSetLayout frameOnlyLayouts[] = { m_frameSetLayout };
    const vk::DescriptorSetLayout fxLayouts[] = { m_frameSetLayout, m_textureSetLayout, m_textureSetLayout, m_textureSetLayout };

    const vk::PushConstantRange shadowPushRange{ vk::ShaderStageFlagBits::eVertex, 0, sizeof(ShadowPushConstants) };
    const vk::PushConstantRange gizmoPushRange{ vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4) };
    const vk::PushConstantRange fxPushRange{ vk::ShaderStageFlagBits::eFragment, 0, kFxPushConstantSize };

    try {
        m_meshShaders = createShaderPair(m_device, "shaders/triangle.vert.spv", "shaders/triangle.frag.spv", meshLayouts);
        m_prepassShaders = createShaderPair(m_device, "shaders/prepass.vert.spv", "shaders/alpha_test.frag.spv", depthLayouts);
        m_shadowShaders = createShaderPair(m_device, "shaders/shadow.vert.spv", "shaders/alpha_test.frag.spv",
            depthLayouts, { &shadowPushRange, 1 });
        m_gizmoShaders = createShaderPair(m_device, "shaders/gizmo.vert.spv", "shaders/gizmo.frag.spv",
            frameOnlyLayouts, { &gizmoPushRange, 1 });
        m_maskShaders = createShaderPair(m_device, "shaders/mask.vert.spv", "shaders/mask.frag.spv",
            fxLayouts, { &fxPushRange, 1 });

        const std::pair<ShaderPair*, const char*> fullscreen[] = {
            { &m_skyShaders, "shaders/sky.frag.spv" },
            { &m_gridShaders, "shaders/grid.frag.spv" },
            { &m_outlineShaders, "shaders/outline.frag.spv" },
            { &m_aoDepthShaders, "shaders/ao_depth.frag.spv" },
            { &m_aoShaders, "shaders/ao.frag.spv" },
            { &m_aoBlurShaders, "shaders/ao_blur.frag.spv" },
            { &m_bloomDownShaders, "shaders/bloom_down.frag.spv" },
            { &m_bloomUpShaders, "shaders/bloom_up.frag.spv" },
            { &m_compositeShaders, "shaders/composite.frag.spv" },
            { &m_fxaaShaders, "shaders/fxaa.frag.spv" },
            { &m_skyLutShaders, "shaders/sky_lut.frag.spv" },
            { &m_skyIrradianceShaders, "shaders/sky_irradiance.frag.spv" },
        };
        for (const auto& [pair, fragment] : fullscreen)
            *pair = createShaderPair(m_device, "shaders/fullscreen.vert.spv", fragment, fxLayouts, { &fxPushRange, 1 });
    }
    catch (const std::exception& e) {
        LOG_ERROR("Failed to load shaders: " << e.what() << "\n");
        return false;
    }

    vk::PipelineLayoutCreateInfo meshLayoutInfo{};
    meshLayoutInfo.setSetLayouts(meshLayouts);
    vk::PipelineLayoutCreateInfo prepassLayoutInfo{};
    prepassLayoutInfo.setSetLayouts(depthLayouts);
    vk::PipelineLayoutCreateInfo shadowLayoutInfo{};
    shadowLayoutInfo.setSetLayouts(depthLayouts).setPushConstantRanges(shadowPushRange);
    vk::PipelineLayoutCreateInfo gizmoLayoutInfo{};
    gizmoLayoutInfo.setSetLayouts(frameOnlyLayouts).setPushConstantRanges(gizmoPushRange);
    vk::PipelineLayoutCreateInfo fxLayoutInfo{};
    fxLayoutInfo.setSetLayouts(fxLayouts).setPushConstantRanges(fxPushRange);

    return takeResult(m_device.createPipelineLayout(meshLayoutInfo), m_meshLayout, "mesh pipeline layout") &&
        takeResult(m_device.createPipelineLayout(prepassLayoutInfo), m_prepassLayout, "prepass pipeline layout") &&
        takeResult(m_device.createPipelineLayout(shadowLayoutInfo), m_shadowLayout, "shadow pipeline layout") &&
        takeResult(m_device.createPipelineLayout(gizmoLayoutInfo), m_gizmoLayout, "gizmo pipeline layout") &&
        takeResult(m_device.createPipelineLayout(fxLayoutInfo), m_fxLayout, "effects pipeline layout");
}

bool Renderer::createLineBuffer(const std::vector<GizmoVertex>& vertices, LineBuffer& out)
{
    VkBufferCreateInfo bufferInfo{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.size = sizeof(GizmoVertex) * vertices.size();
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    allocInfo.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    LineBuffer created;
    if (vmaCreateBuffer(m_allocator, &bufferInfo, &allocInfo, &created.buffer, &created.allocation, nullptr) != VK_SUCCESS) {
        LOG_ERROR("Failed to create line vertex buffer\n");
        return false;
    }
    void* mapped = nullptr;
    if (vmaMapMemory(m_allocator, created.allocation, &mapped) != VK_SUCCESS) {
        LOG_ERROR("Failed to map line vertex buffer\n");
        vmaDestroyBuffer(m_allocator, created.buffer, created.allocation);
        return false;
    }
    memcpy(mapped, vertices.data(), bufferInfo.size);
    vmaUnmapMemory(m_allocator, created.allocation);

    created.vertexCount = static_cast<uint32_t>(vertices.size());
    out = created;
    return true;
}

void Renderer::destroyLineBuffer(LineBuffer& buffer)
{
    if (buffer.buffer != VK_NULL_HANDLE)
        vmaDestroyBuffer(m_allocator, buffer.buffer, buffer.allocation);
    buffer = {};
}

bool Renderer::initImGuiBackend()
{
    const vk::DescriptorPoolSize imguiPoolSize{ vk::DescriptorType::eCombinedImageSampler, 1 };
    vk::DescriptorPoolCreateInfo imguiPoolInfo{ vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet, 1, 1, &imguiPoolSize };
    if (!takeResult(m_device.createDescriptorPool(imguiPoolInfo), m_imguiDescriptorPool, "ImGui descriptor pool"))
        return false;

    // ImGui draws in the single-sample final pass, which has only the swapchain color attachment.
    const VkFormat colorFormat = static_cast<VkFormat>(m_swapchain.format());
    VkPipelineRenderingCreateInfoKHR renderingInfo{ VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &colorFormat;

    const VkInstance instance = m_context->instance();
    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.ApiVersion = m_context->apiVersion();
    initInfo.Instance = instance;
    initInfo.PhysicalDevice = m_context->physicalDevice();
    initInfo.Device = m_device;
    initInfo.QueueFamily = m_context->graphicsQueueFamily();
    initInfo.Queue = m_context->graphicsQueue();
    initInfo.DescriptorPool = m_imguiDescriptorPool;
    initInfo.MinImageCount = m_swapchain.imageCount();
    initInfo.ImageCount = m_swapchain.imageCount();
    initInfo.UseDynamicRendering = true;
    initInfo.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
    initInfo.PipelineInfoMain.Subpass = 0;
    initInfo.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;

    // Built with IMGUI_IMPL_VULKAN_NO_PROTOTYPES, so the backend loads its entry points through volk's loader.
    // ImGui tries the core dynamic rendering commands before the KHR ones. On a 1.2 device the instance-level
    // vkCmdBeginRendering exists but leads nowhere; the device-level lookup fails instead, so ImGui falls back.
    const bool loaded = ImGui_ImplVulkan_LoadFunctions(m_context->apiVersion(), [](const char* functionName, void* userData) {
        const auto* context = static_cast<const VulkanContext*>(userData);
        if (std::strcmp(functionName, "vkCmdBeginRendering") == 0 || std::strcmp(functionName, "vkCmdEndRendering") == 0)
            return vkGetDeviceProcAddr(context->device(), functionName);
        return vkGetInstanceProcAddr(context->instance(), functionName);
    }, m_context);
    if (!loaded || !ImGui_ImplVulkan_Init(&initInfo)) {
        LOG_ERROR("Failed to initialize the ImGui Vulkan backend\n");
        return false;
    }
    m_imguiInitialized = true;
    return true;
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

void Renderer::shutdown()
{
    if (!m_device)
        return;
    (void)m_device.waitIdle();

    if (m_imguiInitialized) {
        ImGui_ImplVulkan_Shutdown();
        m_imguiInitialized = false;
    }
    if (m_imguiDescriptorPool) {
        m_device.destroyDescriptorPool(m_imguiDescriptorPool);
        m_imguiDescriptorPool = nullptr;
    }

    destroyLineBuffer(m_pathLines);

    for (ShaderPair* pair : { &m_meshShaders, &m_prepassShaders, &m_shadowShaders, &m_gizmoShaders, &m_skyShaders,
             &m_gridShaders, &m_maskShaders, &m_outlineShaders, &m_aoDepthShaders, &m_aoShaders, &m_aoBlurShaders,
             &m_bloomDownShaders, &m_bloomUpShaders, &m_compositeShaders, &m_fxaaShaders, &m_skyLutShaders,
             &m_skyIrradianceShaders })
        destroyShaderPair(m_device, *pair);
    for (vk::PipelineLayout* layout : { &m_meshLayout, &m_prepassLayout, &m_shadowLayout, &m_gizmoLayout, &m_fxLayout }) {
        if (*layout) m_device.destroyPipelineLayout(*layout);
        *layout = nullptr;
    }

    m_frameDrawBuffers.clear();
    for (UBOBuffer& ubo : m_frameUBOs)
        if (ubo.buffer) vmaDestroyBuffer(m_allocator, ubo.buffer, ubo.allocation);
    m_frameUBOs.clear();

    for (vk::Semaphore semaphore : m_imageAvailableSemaphores)
        if (semaphore) m_device.destroySemaphore(semaphore);
    for (vk::Semaphore semaphore : m_renderFinishedSemaphores)
        if (semaphore) m_device.destroySemaphore(semaphore);
    for (vk::Fence fence : m_inFlightFences)
        if (fence) m_device.destroyFence(fence);
    m_imageAvailableSemaphores.clear();
    m_renderFinishedSemaphores.clear();
    m_inFlightFences.clear();

    // Destroying the pool frees every set allocated from it, including ModelManager's.
    if (m_descriptorPool) m_device.destroyDescriptorPool(m_descriptorPool);
    m_descriptorPool = nullptr;
    m_frameSets.clear();
    for (vk::DescriptorSet* set : { &m_lightingSet, &m_sceneDepthSet, &m_selectionMaskSet, &m_hdrSet, &m_ldrSet,
             &m_aoDepthSet, &m_aoRawSet, &m_aoTempSet, &m_skyLutSet })
        *set = nullptr;
    m_bloomSets = {};
    for (vk::DescriptorSetLayout* layout : { &m_frameSetLayout, &m_textureSetLayout, &m_lightingSetLayout }) {
        if (*layout) m_device.destroyDescriptorSetLayout(*layout);
        *layout = nullptr;
    }
    for (vk::Sampler* sampler : { &m_textureSampler, &m_nearestSampler, &m_linearClampSampler, &m_skySampler }) {
        if (*sampler) m_device.destroySampler(*sampler);
        *sampler = nullptr;
    }

    if (m_commandPool) m_device.destroyCommandPool(m_commandPool);
    m_commandPool = nullptr;
    m_commandBuffers.clear();

    destroyRenderTargets();
    destroySkyResources();
    destroyShadowMap(m_shadowMap, m_allocator, m_device);
    m_swapchain.destroy();

    m_device = nullptr;
    m_allocator = VK_NULL_HANDLE;
    m_context = nullptr;
    m_window = nullptr;
}

// ---------------------------------------------------------------------------
// Settings / swapchain / path lines
// ---------------------------------------------------------------------------

void Renderer::applySettings(const GraphicsSettings& requested)
{
    const GraphicsSettings settings = sanitizeGraphicsSettings(requested);
    if (settings == m_settings || !m_device)
        return;
    const GraphicsSettings old = m_settings;
    m_settings = settings;
    updateSun();

    if (old.vsync != settings.vsync)
        m_swapchainDirty = true;

    if (effectiveSampleCount() != m_samples) {
        (void)m_device.waitIdle();
        destroyRenderTargets();
        if (!createRenderTargets())
            LOG_ERROR("Failed to recreate render targets for MSAA " << settings.msaaSamples << "x\n");
    }

    if (old.shadowMapSize != settings.shadowMapSize || old.shadowCascades != settings.shadowCascades) {
        (void)m_device.waitIdle();
        destroyShadowMap(m_shadowMap, m_allocator, m_device);
        try {
            createShadowMapResources();
        }
        catch (const std::exception& e) {
            LOG_ERROR(e.what() << "; falling back to 1024\n");
            m_settings.shadowMapSize = 1024;
            createShadowMapResources();
        }
    }
    // Sun direction, shadow distance and caster changes are caught by the per-cascade hashes.
    if (old.shadows != settings.shadows)
        m_shadowMapValid = false;
}

void Renderer::updateSun()
{
    m_sun.direction = sunDirectionFromAngles(m_settings.sunAzimuth, m_settings.sunElevation);
    m_sun.topIrradiance = srgbToLinear(m_settings.sunColor) * (m_settings.sunIntensity * kSunIrradiance);
    m_sun.groundIrradiance = m_settings.sun
        ? m_sun.topIrradiance * sunTransmittance(-m_sun.direction, m_settings.haze)
        : glm::vec3(0.0f);
}

bool Renderer::shadowsActive() const
{
    // Nothing to shadow once the sun is off, set or has no intensity.
    return m_settings.shadows && glm::dot(m_sun.groundIrradiance, glm::vec3(1.0f)) > 1e-4f;
}

bool Renderer::contactShadowsActive() const
{
    return m_settings.contactShadows && glm::dot(m_sun.groundIrradiance, glm::vec3(1.0f)) > 1e-4f;
}

bool Renderer::depthPrepassEnabled() const
{
    return m_settings.ambientOcclusion > 0 || contactShadowsActive();
}

uint64_t Renderer::skyHash() const
{
    Hasher hash;
    const float values[] = { m_sun.direction.x, m_sun.direction.y, m_sun.direction.z, m_sun.topIrradiance.r,
        m_sun.topIrradiance.g, m_sun.topIrradiance.b, m_settings.haze, kGroundAlbedo };
    hash.bytes(values, sizeof(values));
    return hash.value();
}

bool Renderer::prepareSwapchain()
{
    if (!m_swapchainDirty)
        return true;
    if (!recreateSwapchain())
        return false;
    m_swapchainDirty = false;
    return true;
}

bool Renderer::recreateSwapchain()
{
    int pixelWidth = 0, pixelHeight = 0;
    SDL_GetWindowSizeInPixels(m_window, &pixelWidth, &pixelHeight);
    if (pixelWidth <= 0 || pixelHeight <= 0)
        return false;

    // Waits for the device to go idle, so the old render targets below are no longer in use.
    if (!m_swapchain.recreate(static_cast<uint32_t>(pixelWidth), static_cast<uint32_t>(pixelHeight), m_settings.vsync))
        return false;

    while (m_renderFinishedSemaphores.size() < m_swapchain.imageCount()) {
        vk::Semaphore semaphore;
        if (!takeResult(m_device.createSemaphore({}), semaphore, "semaphore"))
            return false;
        m_renderFinishedSemaphores.push_back(semaphore);
    }

    destroyRenderTargets();
    return createRenderTargets();
}

void Renderer::setPathLines(const std::vector<GizmoVertex>& vertices)
{
    if (m_pathLines.buffer != VK_NULL_HANDLE) {
        // Frames still in flight may be reading the old buffer.
        (void)m_device.waitIdle();
        destroyLineBuffer(m_pathLines);
    }
    if (!vertices.empty())
        createLineBuffer(vertices, m_pathLines);
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

Renderer::FrameStatus Renderer::renderFrame(const FrameInput& input)
{
    const vk::Fence fence = m_inFlightFences[m_currentFrame];
    (void)m_device.waitForFences(fence, VK_TRUE, UINT64_MAX);

    uint32_t imageIndex = 0;
    // Pointer overload: returns the raw result instead of asserting on eErrorOutOfDateKHR.
    const vk::Result result = m_device.acquireNextImageKHR(m_swapchain.handle(), UINT64_MAX,
        m_imageAvailableSemaphores[m_currentFrame], {}, &imageIndex);
    if (result == vk::Result::eErrorOutOfDateKHR) {
        m_swapchainDirty = true;
        return FrameStatus::Skipped;
    }
    if (result != vk::Result::eSuccess && result != vk::Result::eSuboptimalKHR) {
        LOG_ERROR("Failed to acquireNextImageKHR\n");
        return FrameStatus::Failed;
    }
    if (result == vk::Result::eSuboptimalKHR)
        m_swapchainDirty = true;

    // Reset only once we know this frame will be submitted; otherwise the next wait would hang.
    (void)m_device.resetFences(fence);

    const glm::vec2 depthRange = projectionDepthRange(input.proj);
    computeShadowCascades(input.view, input.proj, depthRange.x, m_settings.shadowDistance, m_sun.direction,
        m_shadowMap.size, std::span(m_cascades.data(), m_shadowMap.layers));

    const FrameUBO frameData = buildFrameUBO(input);
    memcpy(m_frameUBOs[m_currentFrame].mapped, &frameData, sizeof(FrameUBO));

    FrameBatches batches;
    std::array<uint64_t, kMaxShadowCascades> shadowHashes{};
    cullAndBatch(input, frameData.proj * frameData.view, batches, shadowHashes);
    // A cascade whose casters and matrix did not change keeps last frame's contents.
    for (uint32_t c = 0; c < m_shadowMap.layers; ++c) {
        m_renderCascade[c] = !m_shadowMapValid || shadowHashes[c] != m_cascadeHashes[c];
        m_cascadeHashes[c] = shadowHashes[c];
    }
    m_shadowMapValid = true;

    const uint64_t sky = skyHash();
    const bool renderSky = !m_skyValid || sky != m_skyHash;
    m_skyHash = sky;
    m_skyValid = true;

    buildDrawStreams(input, batches);
    uploadDrawStreams();

    const vk::CommandBuffer cmd = m_commandBuffers[m_currentFrame];
    (void)cmd.reset();
    (void)cmd.begin({ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });

    if (renderSky)
        recordSkyPasses(cmd);
    recordShadowPasses(cmd, *input.models);

    const bool depthPrepass = depthPrepassEnabled();
    transitionFrameTargets(cmd, depthPrepass);
    if (depthPrepass) {
        recordDepthPrepass(cmd, input);
        recordAoPasses(cmd, input);
    }
    recordScenePass(cmd, input, depthPrepass);
    const bool drawOutline = !m_highlightRuns.empty();
    if (drawOutline)
        recordSelectionMask(cmd, input);
    if (m_settings.bloom)
        recordBloom(cmd, input);
    recordFinalPass(cmd, imageIndex, input, drawOutline);

    (void)cmd.end();
    ++m_frameIndex;
    return submitAndPresent(cmd, imageIndex);
}

FrameUBO Renderer::buildFrameUBO(const FrameInput& input) const
{
    FrameUBO frameData{};
    frameData.view = input.view;
    frameData.proj = input.proj;
    frameData.invViewProj = glm::inverse(input.proj * input.view);

    for (uint32_t c = 0; c < kMaxShadowCascades; ++c) {
        const ShadowCascade& cascade = m_cascades[c];
        frameData.cascadeMatrices[c] = cascade.matrix;
        frameData.cascadeSplits[c] = cascade.splitEnd;
        frameData.cascadeParams[c] = glm::vec4(cascade.texelWorld, cascade.depthRange,
            1.0f / (2.0f * std::max(cascade.radius, 1e-3f)), 0.0f);
    }

    frameData.cameraPos = glm::vec4(input.cameraPosition, 1.0f);
    frameData.lightDir = glm::vec4(m_sun.direction, kSunAngularRadius);
    frameData.sunColor = glm::vec4(m_sun.groundIrradiance, 0.0f);
    frameData.sunTopColor = glm::vec4(m_sun.topIrradiance, 0.0f);
    const bool realistic = m_settings.background == BackgroundMode::Realistic;
    frameData.backgroundColor = glm::vec4(srgbToLinear(m_settings.backgroundColor), realistic ? 1.0f : 0.0f);
    frameData.atmosphereParams = glm::vec4(m_settings.haze, kGroundAlbedo, 0.0f, 0.0f);

    const glm::vec2 depthRange = projectionDepthRange(input.proj);
    frameData.fogParams = glm::vec4(m_settings.fog ? 1.0f : 0.0f, kFogDensity * m_settings.fogDensity,
        kFogHeightFalloff, depthRange.y * 0.8f);
    frameData.shadowParams = glm::vec4(shadowsActive() ? 1.0f : 0.0f, static_cast<float>(m_shadowMap.layers),
        m_settings.shadowDistance, m_settings.softShadows ? 1.0f : 0.0f);
    frameData.shadowParams2 = glm::vec4(1.0f / static_cast<float>(std::max(m_shadowMap.size, 1u)),
        kPenumbraPerMeter, 0.0f, 0.0f);
    frameData.aoParams = glm::vec4(m_settings.ambientOcclusion > 0 ? 1.0f : 0.0f, m_settings.aoRadius,
        m_settings.aoIntensity, static_cast<float>(kAoSamples[m_settings.ambientOcclusion]));
    frameData.aoParams2 = glm::vec4(contactShadowsActive() ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
    frameData.projParams = glm::vec4(1.0f / input.proj[0][0], 1.0f / input.proj[1][1], 0.0f, 0.0f);

    const vk::Rect2D rect = sceneRect(input);
    frameData.viewport = glm::vec4(rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height);
    const vk::Extent2D extent = m_swapchain.extent();
    frameData.renderSize = glm::vec4(extent.width, extent.height, 1.0f / extent.width, 1.0f / extent.height);

    frameData.time = input.time;
    frameData.nearPlane = depthRange.x;
    frameData.farPlane = depthRange.y;
    frameData.frameIndex = m_frameIndex;
    return frameData;
}

void Renderer::cullAndBatch(const FrameInput& input, const glm::mat4& viewProj, FrameBatches& batches,
    std::array<uint64_t, kMaxShadowCascades>& shadowHashes)
{
    ModelManager& models = *input.models;
    const bool shadows = shadowsActive();
    const uint32_t cascadeCount = shadows ? m_shadowMap.layers : 0;
    const auto& instances = models.getInstances();
    m_cullResults.resize(instances.size());
    m_cullIndices.resize(instances.size());
    std::iota(m_cullIndices.begin(), m_cullIndices.end(), 0);

    // 1. Per-instance phase: transform + whole-model culling.
    std::for_each(std::execution::par, m_cullIndices.begin(), m_cullIndices.end(), [&](size_t i) {
        const auto& inst = instances[i];
        auto& res = m_cullResults[i];

        res.instance = &inst;
        res.modelIndex = inst.modelIndex;
        res.gpuModel = nullptr;
        res.visibleMain = false;
        res.shadowMask = 0;

        if (!inst.visible) return;

        GPUModel* gpuModel = models.getModel(inst.modelIndex);
        if (!gpuModel || !gpuModel->isValid()) return;

        res.gpuModel = gpuModel;
        res.transform = inst.getTransformMatrix();
        res.maxScale = std::max({ std::abs(inst.scale.x), std::abs(inst.scale.y), std::abs(inst.scale.z) });

        res.mainPlanes = extractFrustumPlanes(viewProj * res.transform);
        res.visibleMain = isAABBInFrustum(res.mainPlanes, gpuModel->boundsMin, gpuModel->boundsMax);

        for (uint32_t c = 0; c < cascadeCount; ++c) {
            FrustumPlanes planes = extractFrustumPlanes(m_cascades[c].matrix * res.transform);
            // Casters between the sun and the cascade still throw shadows into it (depth clamp flattens them).
            planes.planes[4] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
            if (isAABBInFrustum(planes, gpuModel->boundsMin, gpuModel->boundsMax)) {
                res.shadowPlanes[c] = planes;
                res.shadowMask |= static_cast<uint8_t>(1u << c);
            }
        }

        res.submeshFlags.assign(gpuModel->submeshes.size(), 0);
    });

    // 2. Per-submesh phase, split into chunks so a single huge model still uses all cores.
    m_cullChunks.clear();
    for (size_t i = 0; i < m_cullResults.size(); ++i) {
        const auto& res = m_cullResults[i];
        if (!res.gpuModel || !(res.visibleMain || res.shadowMask)) continue;
        const size_t n = res.gpuModel->submeshes.size();
        for (size_t b = 0; b < n; b += kCullChunkSize)
            m_cullChunks.push_back({ i, b, std::min(b + kCullChunkSize, n) });
    }

    std::for_each(std::execution::par, m_cullChunks.begin(), m_cullChunks.end(), [&](const CullChunk& chunk) {
        auto& res = m_cullResults[chunk.instanceIdx];
        const auto& submeshes = res.gpuModel->submeshes;
        const IfcScene* ifc = res.instance->ifcScene ? &*res.instance->ifcScene : nullptr;

        for (size_t s = chunk.begin; s < chunk.end; ++s) {
            if (ifc && !ifc->isSubmeshVisible(s)) continue;

            const SubmeshInfo& sub = submeshes[s];
            const bool validBounds = sub.boundsMin.x <= sub.boundsMax.x;
            uint8_t flags = 0;
            if (res.visibleMain &&
                (!validBounds || isAABBInFrustum(res.mainPlanes, sub.boundsMin, sub.boundsMax)))
                flags |= kVisibleMain;
            if (res.shadowMask && sub.material.alphaMode != AlphaMode::BLEND) {
                const float radius = validBounds
                    ? 0.5f * glm::length(sub.boundsMax - sub.boundsMin) * res.maxScale
                    : std::numeric_limits<float>::max();
                for (uint32_t c = 0; c < cascadeCount; ++c) {
                    // Casters smaller than a texel would only add aliasing noise to a cascade.
                    if (!(res.shadowMask & (1u << c)) || radius < m_cascades[c].texelWorld)
                        continue;
                    if (!validBounds || isAABBInFrustum(res.shadowPlanes[c], sub.boundsMin, sub.boundsMax))
                        flags |= static_cast<uint8_t>(kVisibleShadow << c);
                }
            }
            res.submeshFlags[s] = flags;
        }
    });

    // 3. Sequential aggregation, hashing everything that affects each cascade on the way.
    std::array<Hasher, kMaxShadowCascades> hashers;
    for (uint32_t c = 0; c < m_shadowMap.layers; ++c) {
        hashers[c].byte(static_cast<uint8_t>(shadows ? 1 : 0));
        hashers[c].bytes(&m_shadowMap.size, sizeof(m_shadowMap.size));
        if (shadows)
            hashers[c].bytes(&m_cascades[c].matrix, sizeof(glm::mat4));
    }

    m_frameTransforms.clear();
    for (const auto& res : m_cullResults) {
        if (!res.gpuModel || !(res.visibleMain || res.shadowMask)) continue;

        const uint32_t transformIndex = pushTransform(res.transform);
        InstanceRenderData renderData{ res.instance, res.transform, res.submeshFlags.data(), transformIndex };

        if (res.visibleMain) {
            auto& batch = batches.main[res.modelIndex];
            batch.model = res.gpuModel;
            batch.instances.push_back(renderData);
        }

        if (res.shadowMask) {
            auto& batch = batches.shadow[res.modelIndex];
            batch.model = res.gpuModel;
            batch.instances.push_back(renderData);

            for (uint32_t c = 0; c < cascadeCount; ++c) {
                if (!(res.shadowMask & (1u << c))) continue;
                Hasher& hash = hashers[c];
                hash.bytes(&res.gpuModel, sizeof(res.gpuModel));
                hash.bytes(&res.transform, sizeof(glm::mat4));
                const uint8_t bit = static_cast<uint8_t>(kVisibleShadow << c);
                for (uint8_t f : res.submeshFlags)
                    hash.byte(f & bit);
            }
        }
    }
    for (uint32_t c = 0; c < m_shadowMap.layers; ++c)
        shadowHashes[c] = hashers[c].value();
}

void Renderer::materialSets(const ModelManager& models, const GPUModel* model, const Material& material,
    vk::DescriptorSet out[3]) const
{
    auto pick = [&](int texIndex, vk::DescriptorSet fallback) {
        return (texIndex >= 0 && texIndex < static_cast<int>(model->textureDescriptorSets.size()))
            ? model->textureDescriptorSets[texIndex] : fallback;
    };
    out[0] = pick(material.baseColorTextureIndex, models.getDefaultBaseColorSet());
    out[1] = pick(material.normalTextureIndex, models.getDefaultNormalSet());
    out[2] = pick(material.metallicRoughnessTextureIndex, models.getDefaultMRSet());
}

uint32_t Renderer::pushTransform(const glm::mat4& transform)
{
    m_frameTransforms.push_back({ transform, glm::mat4(glm::transpose(glm::inverse(glm::mat3(transform)))) });
    return static_cast<uint32_t>(m_frameTransforms.size() - 1);
}

uint32_t Renderer::pushDrawData(const SubmeshInfo& sub, uint32_t transformIndex, const glm::vec3& tint)
{
    GpuDrawData d{};
    d.baseColor = sub.material.baseColorFactor * glm::vec4(tint, 1.0f);
    d.transformIndex = transformIndex;
    d.alphaMode = static_cast<int32_t>(sub.material.alphaMode);
    d.metallic = sub.material.metallicFactor;
    d.roughness = sub.material.roughnessFactor;
    d.alphaCutoff = sub.material.alphaCutoff;
    m_frameDraws.push_back(d);
    return static_cast<uint32_t>(m_frameDraws.size() - 1);
}

void Renderer::appendDraw(std::vector<DrawRun>& runs, GPUModel* model, const vk::DescriptorSet sets[3], bool blend,
    const SubmeshInfo& sub, uint32_t transformIndex, const glm::vec3& tint)
{
    const uint32_t commandIndex = static_cast<uint32_t>(m_frameCommands.size());
    const uint32_t drawIndex = pushDrawData(sub, transformIndex, tint);
    m_frameCommands.push_back(vk::DrawIndexedIndirectCommand(
        sub.indexCount, 1, sub.indexOffset, static_cast<int32_t>(sub.vertexOffset), drawIndex));

    if (!runs.empty()) {
        DrawRun& run = runs.back();
        bool compatible = run.model == model && run.blend == blend &&
            run.firstCommand + run.commandCount == commandIndex;
        for (int k = 0; k < 3 && compatible; ++k)
            compatible = !sets[k] || !run.sets[k] || sets[k] == run.sets[k];
        if (compatible) {
            for (int k = 0; k < 3; ++k)
                if (!run.sets[k]) run.sets[k] = sets[k];
            ++run.commandCount;
            return;
        }
    }
    runs.push_back({ model, { sets[0], sets[1], sets[2] }, blend, commandIndex, 1 });
}

// Every visible (instance, submesh) pair becomes one GpuDrawData entry plus one indirect command whose
// firstInstance points at that entry; compatible neighbours are merged into DrawRuns.
void Renderer::buildDrawStreams(const FrameInput& input, FrameBatches& batches)
{
    ModelManager& models = *input.models;
    const glm::vec3 cameraPos = input.cameraPosition;

    m_frameDraws.clear();
    m_frameCommands.clear();
    for (auto& runs : m_shadowRuns)
        runs.clear();
    m_opaqueRuns.clear();
    m_blendRuns.clear();

    // Only cascades that are re-rendered this frame need draws.
    for (uint32_t c = 0; c < m_shadowMap.layers; ++c) {
        if (!m_renderCascade[c]) continue;
        const uint8_t bit = static_cast<uint8_t>(kVisibleShadow << c);
        for (auto& [modelIdx, batch] : batches.shadow) {
            GPUModel* model = batch.model;
            for (uint32_t si : model->drawOrder) {
                const SubmeshInfo& sub = model->submeshes[si];
                // Only alpha-masked submeshes need a specific texture in the shadow pass.
                vk::DescriptorSet sets[3] = {};
                if (sub.material.alphaMode == AlphaMode::MASK) {
                    vk::DescriptorSet all[3];
                    materialSets(models, model, sub.material, all);
                    sets[0] = all[0];
                }
                for (const auto& rd : batch.instances)
                    if (rd.submeshFlags[si] & bit)
                        appendDraw(m_shadowRuns[c], model, sets, false, sub, rd.transformIndex, glm::vec3(1.0f));
            }
        }
    }

    for (auto& [modelIdx, batch] : batches.main) {
        GPUModel* model = batch.model;
        for (uint32_t si : model->drawOrder) {
            const SubmeshInfo& sub = model->submeshes[si];
            if (sub.material.alphaMode == AlphaMode::BLEND) continue;
            vk::DescriptorSet sets[3];
            materialSets(models, model, sub.material, sets);
            for (const auto& rd : batch.instances)
                if (rd.submeshFlags[si] & kVisibleMain)
                    appendDraw(m_opaqueRuns, model, sets, false, sub, rd.transformIndex, rd.instance->color);
        }
    }

    std::vector<InstanceRenderData> sortedInstances;
    for (auto& [modelIdx, batch] : batches.main) {
        GPUModel* model = batch.model;
        bool sorted = false;
        for (std::size_t si = 0; si < model->submeshes.size(); ++si) {
            const SubmeshInfo& sub = model->submeshes[si];
            if (sub.material.alphaMode != AlphaMode::BLEND) continue;

            if (!sorted) {
                sortedInstances = batch.instances;
                std::sort(sortedInstances.begin(), sortedInstances.end(), [&](const InstanceRenderData& a, const InstanceRenderData& b) {
                    return glm::distance(cameraPos, a.instance->position) > glm::distance(cameraPos, b.instance->position);
                });
                sorted = true;
            }

            vk::DescriptorSet sets[3];
            materialSets(models, model, sub.material, sets);
            for (const auto& rd : sortedInstances)
                if (rd.submeshFlags[si] & kVisibleMain)
                    appendDraw(m_blendRuns, model, sets, true, sub, rd.transformIndex, rd.instance->color);
        }
    }

    buildHighlightStream(input);
}

// The selection mask draws the highlighted submeshes whether or not they passed culling, so the
// outline of partly off-screen objects stays correct.
void Renderer::buildHighlightStream(const FrameInput& input)
{
    m_highlightRuns.clear();
    const SelectionHighlight& highlight = input.highlight;
    ModelManager& models = *input.models;
    const auto& instances = models.getInstances();
    if (highlight.instance < 0 || highlight.instance >= static_cast<int>(instances.size()))
        return;
    const ModelInstance& inst = instances[highlight.instance];
    GPUModel* model = models.getModel(inst.modelIndex);
    if (!inst.visible || !model || !model->isValid())
        return;
    if (!highlight.wholeInstance && highlight.submeshes.empty())
        return;

    const IfcScene* ifc = inst.ifcScene ? &*inst.ifcScene : nullptr;
    const uint32_t transformIndex = pushTransform(inst.getTransformMatrix());
    const vk::DescriptorSet noSets[3] = {};
    auto add = [&](size_t si) {
        if (si >= model->submeshes.size() || (ifc && !ifc->isSubmeshVisible(si)))
            return;
        appendDraw(m_highlightRuns, model, noSets, false, model->submeshes[si], transformIndex, glm::vec3(1.0f));
    };
    if (highlight.wholeInstance) {
        for (size_t si = 0; si < model->submeshes.size(); ++si)
            add(si);
    }
    else {
        for (uint32_t si : highlight.submeshes)
            add(si);
    }
}

void Renderer::uploadDrawStreams()
{
    // Safe to grow/rewrite: this frame's fence was waited on, so the GPU no longer reads these
    // buffers or this frame's descriptor set.
    FrameDrawBuffers& buffers = *m_frameDrawBuffers[m_currentFrame];
    const vk::DeviceSize drawBytes = sizeof(GpuDrawData) * m_frameDraws.size();
    const vk::DeviceSize transformBytes = sizeof(GpuTransform) * m_frameTransforms.size();
    const vk::DeviceSize commandBytes = sizeof(vk::DrawIndexedIndirectCommand) * m_frameCommands.size();
    bool descriptorsStale = buffers.draws.reserve(drawBytes);
    descriptorsStale = buffers.transforms.reserve(transformBytes) || descriptorsStale;
    buffers.indirect.reserve(commandBytes);
    if (descriptorsStale) writeDrawDescriptors(m_currentFrame);

    if (drawBytes) memcpy(buffers.draws.mapped(), m_frameDraws.data(), drawBytes);
    if (transformBytes) memcpy(buffers.transforms.mapped(), m_frameTransforms.data(), transformBytes);
    if (commandBytes) memcpy(buffers.indirect.mapped(), m_frameCommands.data(), commandBytes);
}

// ---------------------------------------------------------------------------
// Recording
// ---------------------------------------------------------------------------

void Renderer::setDefaultDrawState(vk::CommandBuffer cmd, vk::SampleCountFlagBits samples,
    const vk::Viewport& viewport, const vk::Rect2D& scissor) const
{
    cmd.setViewportWithCount(1, &viewport);
    cmd.setScissorWithCount(1, &scissor);
    cmd.setRasterizerDiscardEnable(VK_FALSE);
    cmd.setCullMode(vk::CullModeFlagBits::eNone);
    cmd.setFrontFace(vk::FrontFace::eCounterClockwise);
    cmd.setDepthTestEnable(VK_FALSE);
    cmd.setDepthWriteEnable(VK_FALSE);
    cmd.setDepthCompareOp(vk::CompareOp::eLessOrEqual);
    cmd.setDepthBiasEnable(VK_FALSE);
    cmd.setDepthClampEnableEXT(VK_FALSE);
    cmd.setStencilTestEnable(VK_FALSE);
    cmd.setPolygonModeEXT(vk::PolygonMode::eFill);
    cmd.setRasterizationSamplesEXT(samples);
    const vk::SampleMask sampleMask = ~0u;
    cmd.setSampleMaskEXT(samples, &sampleMask);
    cmd.setAlphaToCoverageEnableEXT(VK_FALSE);
    cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
    cmd.setPrimitiveRestartEnable(VK_FALSE);
    cmd.setColorWriteMaskEXT(0, vk::ColorComponentFlags(0xF));
    setAlphaBlending(cmd, false);
}

void Renderer::setAlphaBlending(vk::CommandBuffer cmd, bool enabled) const
{
    cmd.setColorBlendEnableEXT(0, enabled ? VK_TRUE : VK_FALSE);
    vk::ColorBlendEquationEXT blendEquation{};
    blendEquation.srcColorBlendFactor = vk::BlendFactor::eSrcAlpha;
    blendEquation.dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendEquation.colorBlendOp = vk::BlendOp::eAdd;
    blendEquation.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendEquation.dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha;
    blendEquation.alphaBlendOp = vk::BlendOp::eAdd;
    cmd.setColorBlendEquationEXT(0, 1, &blendEquation);
}

void Renderer::setAdditiveBlending(vk::CommandBuffer cmd) const
{
    cmd.setColorBlendEnableEXT(0, VK_TRUE);
    vk::ColorBlendEquationEXT blendEquation{};
    blendEquation.srcColorBlendFactor = vk::BlendFactor::eOne;
    blendEquation.dstColorBlendFactor = vk::BlendFactor::eOne;
    blendEquation.colorBlendOp = vk::BlendOp::eAdd;
    blendEquation.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    blendEquation.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    blendEquation.alphaBlendOp = vk::BlendOp::eAdd;
    cmd.setColorBlendEquationEXT(0, 1, &blendEquation);
}

// The editor's scene area in framebuffer pixels (window coordinates are scaled on high-DPI displays).
vk::Rect2D Renderer::sceneRect(const FrameInput& input) const
{
    const vk::Extent2D extent = m_swapchain.extent();
    const int32_t fbWidth = static_cast<int32_t>(extent.width);
    const int32_t fbHeight = static_cast<int32_t>(extent.height);
    const ViewRect& view = input.viewport;
    const float pixelScaleX = float(extent.width) / input.windowWidth;
    const float pixelScaleY = float(extent.height) / input.windowHeight;
    const int32_t x0 = std::clamp(static_cast<int32_t>(std::lround(view.x * pixelScaleX)), 0, fbWidth - 1);
    const int32_t y0 = std::clamp(static_cast<int32_t>(std::lround(view.y * pixelScaleY)), 0, fbHeight - 1);
    const int32_t x1 = std::clamp(static_cast<int32_t>(std::lround((view.x + view.width) * pixelScaleX)), x0 + 1, fbWidth);
    const int32_t y1 = std::clamp(static_cast<int32_t>(std::lround((view.y + view.height) * pixelScaleY)), y0 + 1, fbHeight);
    return { { x0, y0 }, { uint32_t(x1 - x0), uint32_t(y1 - y0) } };
}

void Renderer::bindModelBuffers(vk::CommandBuffer cmd, const GPUModel* model) const
{
    const vk::Buffer buffers[1] = { model->vertexBuffer->getBuffer() };
    const vk::DeviceSize offsets[1] = { 0 };
    const vk::DeviceSize sizes[1] = { sizeof(Vertex) * model->vertexCount };
    const vk::DeviceSize strides[1] = { sizeof(Vertex) };
    cmd.bindVertexBuffers2(0, 1, buffers, offsets, sizes, strides);
    cmd.bindIndexBuffer(model->indexBuffer->getBuffer(), 0, vk::IndexType::eUint32);
}

void Renderer::recordRun(vk::CommandBuffer cmd, const DrawRun& run) const
{
    constexpr uint32_t stride = sizeof(vk::DrawIndexedIndirectCommand);
    const vk::Buffer indirect = m_frameDrawBuffers[m_currentFrame]->indirect.getBuffer();
    for (uint32_t done = 0; done < run.commandCount;) {
        const uint32_t count = std::min(run.commandCount - done, m_maxDrawIndirectCount);
        cmd.drawIndexedIndirect(indirect, vk::DeviceSize(run.firstCommand + done) * stride, count, stride);
        done += count;
    }
}

// Depth-only runs: only the base color texture (set 1, for alpha testing) is bound.
void Renderer::recordDepthRuns(vk::CommandBuffer cmd, vk::PipelineLayout layout, const std::vector<DrawRun>& runs,
    const ModelManager& models) const
{
    cmd.setVertexInputEXT(1, &m_meshBinding, static_cast<uint32_t>(m_meshAttributes.size()), m_meshAttributes.data());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, 1, &m_frameSets[m_currentFrame], 0, nullptr);
    // alpha_test.frag statically uses set 1, so it must be bound even when nothing is alpha-masked.
    vk::DescriptorSet boundSet = models.getDefaultBaseColorSet();
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 1, 1, &boundSet, 0, nullptr);

    const GPUModel* boundModel = nullptr;
    for (const DrawRun& run : runs) {
        if (run.model != boundModel) {
            bindModelBuffers(cmd, run.model);
            boundModel = run.model;
        }
        if (run.sets[0] && run.sets[0] != boundSet) {
            boundSet = run.sets[0];
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 1, 1, &boundSet, 0, nullptr);
        }
        recordRun(cmd, run);
    }
}

void Renderer::recordShadowPasses(vk::CommandBuffer cmd, const ModelManager& models)
{
    const uint32_t size = m_shadowMap.size;
    const vk::Image shadowImage(m_shadowMap.image);
    const auto depthAspect = vk::ImageAspectFlagBits::eDepth;
    const auto depthStages = vk::PipelineStageFlagBits2::eEarlyFragmentTests | vk::PipelineStageFlagBits2::eLateFragmentTests;
    const vk::Rect2D area{ { 0, 0 }, { size, size } };

    for (uint32_t c = 0; c < m_shadowMap.layers; ++c) {
        if (!m_renderCascade[c])
            continue;
        // Unchanged cascades keep their contents; this one is redrawn from scratch.
        const vk::ImageMemoryBarrier2 toAttachment = imageBarrier(shadowImage, depthAspect,
            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eNone,
            depthStages, vk::AccessFlagBits2::eDepthStencilAttachmentRead | vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthAttachmentOptimal, 0, 1, c, 1);
        pipelineBarriers(cmd, { &toAttachment, 1 });

        vk::RenderingAttachmentInfo depthAttachment{};
        depthAttachment.setImageView(m_shadowMap.layerViews[c])
            .setImageLayout(vk::ImageLayout::eDepthAttachmentOptimal)
            .setLoadOp(vk::AttachmentLoadOp::eClear)
            .setStoreOp(vk::AttachmentStoreOp::eStore)
            .setClearValue(vk::ClearValue(vk::ClearDepthStencilValue{ 1.0f, 0 }));
        vk::RenderingInfo renderInfo{};
        renderInfo.setRenderArea(area).setLayerCount(1).setPDepthAttachment(&depthAttachment);

        beginRendering(cmd, renderInfo);
        if (!m_shadowRuns[c].empty()) {
            bindShaderPair(cmd, m_shadowShaders);
            setDefaultDrawState(cmd, vk::SampleCountFlagBits::e1, viewportFor(area), area);
            // Both faces cast: single-sided geometry facing the sun must still block it. The normal offset
            // and slope bias keep lit faces from shadowing themselves.
            cmd.setDepthTestEnable(VK_TRUE);
            cmd.setDepthWriteEnable(VK_TRUE);
            cmd.setDepthBiasEnable(VK_TRUE);
            cmd.setDepthBias(1.25f, 0.0f, 1.75f);
            // Casters in front of the cascade are flattened onto its near plane instead of being clipped.
            cmd.setDepthClampEnableEXT(m_context->features().depthClamp ? VK_TRUE : VK_FALSE);
            const ShadowPushConstants push{ c, {} };
            cmd.pushConstants(m_shadowLayout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(push), &push);
            recordDepthRuns(cmd, m_shadowLayout, m_shadowRuns[c], models);
        }
        cmd.endRendering();

        const vk::ImageMemoryBarrier2 toSampled = imageBarrier(shadowImage, depthAspect,
            vk::PipelineStageFlagBits2::eLateFragmentTests, vk::AccessFlagBits2::eDepthStencilAttachmentWrite,
            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
            vk::ImageLayout::eDepthAttachmentOptimal, vk::ImageLayout::eDepthStencilReadOnlyOptimal, 0, 1, c, 1);
        pipelineBarriers(cmd, { &toSampled, 1 });
    }
}

void Renderer::transitionFrameTargets(vk::CommandBuffer cmd, bool depthPrepass)
{
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const auto colorAspect = vk::ImageAspectFlagBits::eColor;
    const auto depthAspect = vk::ImageAspectFlagBits::eDepth;
    const auto depthStages = Stage::eEarlyFragmentTests | Stage::eLateFragmentTests;
    const auto depthAccess = Access::eDepthStencilAttachmentRead | Access::eDepthStencilAttachmentWrite;
    // Earlier frames rendered to these images or sampled them; their contents are discarded.
    const auto previousStages = Stage::eFragmentShader | Stage::eColorAttachmentOutput | depthStages;
    const auto previousWrites = Access::eColorAttachmentWrite | Access::eDepthStencilAttachmentWrite;

    std::vector<vk::ImageMemoryBarrier2> barriers;
    auto toColorTarget = [&](const RenderImage& image, uint32_t mips) {
        barriers.push_back(imageBarrier(vk::Image(image.image), colorAspect, previousStages, previousWrites,
            Stage::eColorAttachmentOutput, Access::eColorAttachmentRead | Access::eColorAttachmentWrite,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal, 0, mips));
    };
    // Inputs of passes skipped this frame must still be in the layout their descriptors promise.
    auto toUnusedInput = [&](const RenderImage& image) {
        barriers.push_back(imageBarrier(vk::Image(image.image), colorAspect, previousStages, previousWrites,
            Stage::eFragmentShader, Access::eShaderSampledRead,
            vk::ImageLayout::eUndefined, vk::ImageLayout::eShaderReadOnlyOptimal));
    };

    // Depth resolves write in the color output stage with color attachment access.
    barriers.push_back(imageBarrier(vk::Image(m_sceneDepth.image), depthAspect, previousStages, previousWrites,
        depthStages | Stage::eColorAttachmentOutput, depthAccess | Access::eColorAttachmentWrite,
        vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthAttachmentOptimal));
    toColorTarget(m_hdrColor, 1);
    if (m_samples != vk::SampleCountFlagBits::e1) {
        toColorTarget(m_msaaColor, 1);
        barriers.push_back(imageBarrier(vk::Image(m_msaaDepth.image), depthAspect, previousStages, previousWrites,
            depthStages, depthAccess, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthAttachmentOptimal));
    }
    if (depthPrepass) {
        toColorTarget(m_aoDepth, 1);
        toColorTarget(m_aoRaw, 1);
        toColorTarget(m_aoTemp, 1);
    }
    else {
        toUnusedInput(m_aoDepth);
        toUnusedInput(m_aoRaw);
    }
    if (m_settings.bloom)
        toColorTarget(m_bloom, m_bloomMips);
    else
        toUnusedInput(m_bloom);
    if (m_settings.fxaa)
        toColorTarget(m_ldrColor, 1);
    pipelineBarriers(cmd, barriers);
}

// Depth of the opaque scene before shading: the AO passes read it, and the scene pass then shades each
// pixel once (depth test against these exact depths, no writes).
void Renderer::recordDepthPrepass(vk::CommandBuffer cmd, const FrameInput& input)
{
    const bool msaa = m_samples != vk::SampleCountFlagBits::e1;
    const vk::Rect2D rect = sceneRect(input);

    vk::RenderingAttachmentInfo depthAttachment{};
    depthAttachment.setImageLayout(vk::ImageLayout::eDepthAttachmentOptimal)
        .setLoadOp(vk::AttachmentLoadOp::eClear)
        .setStoreOp(vk::AttachmentStoreOp::eStore)
        .setClearValue(vk::ClearValue(vk::ClearDepthStencilValue{ 1.0f, 0 }));
    if (msaa) {
        depthAttachment.setImageView(m_msaaDepth.view)
            .setResolveMode(vk::ResolveModeFlagBits::eSampleZero)
            .setResolveImageView(m_sceneDepth.view)
            .setResolveImageLayout(vk::ImageLayout::eDepthAttachmentOptimal);
    }
    else {
        depthAttachment.setImageView(m_sceneDepth.view);
    }
    vk::RenderingInfo renderInfo{};
    renderInfo.setRenderArea(rect).setLayerCount(1).setPDepthAttachment(&depthAttachment);

    beginRendering(cmd, renderInfo);
    if (!m_opaqueRuns.empty()) {
        bindShaderPair(cmd, m_prepassShaders);
        setDefaultDrawState(cmd, m_samples, viewportFor(rect), rect);
        cmd.setDepthTestEnable(VK_TRUE);
        cmd.setDepthWriteEnable(VK_TRUE);
        cmd.setDepthCompareOp(vk::CompareOp::eLess);
        recordDepthRuns(cmd, m_prepassLayout, m_opaqueRuns, *input.models);
    }
    cmd.endRendering();

    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    const auto depthAspect = vk::ImageAspectFlagBits::eDepth;
    const auto depthStages = Stage::eEarlyFragmentTests | Stage::eLateFragmentTests;
    // Read-only from here on: sampled by the AO passes and the selection mask, depth-tested by the scene pass.
    std::vector<vk::ImageMemoryBarrier2> barriers = {
        imageBarrier(vk::Image(m_sceneDepth.image), depthAspect,
            depthStages | Stage::eColorAttachmentOutput,
            Access::eDepthStencilAttachmentWrite | Access::eColorAttachmentWrite,
            Stage::eFragmentShader | depthStages, Access::eShaderSampledRead | Access::eDepthStencilAttachmentRead,
            vk::ImageLayout::eDepthAttachmentOptimal, vk::ImageLayout::eDepthReadOnlyOptimal),
    };
    if (msaa) {
        barriers.push_back(imageBarrier(vk::Image(m_msaaDepth.image), depthAspect,
            depthStages | Stage::eColorAttachmentOutput, Access::eDepthStencilAttachmentWrite,
            depthStages, Access::eDepthStencilAttachmentRead | Access::eDepthStencilAttachmentWrite,
            vk::ImageLayout::eDepthAttachmentOptimal, vk::ImageLayout::eDepthAttachmentOptimal));
    }
    pipelineBarriers(cmd, barriers);
}

void Renderer::recordScenePass(vk::CommandBuffer cmd, const FrameInput& input, bool depthPrepass)
{
    const bool msaa = m_samples != vk::SampleCountFlagBits::e1;
    const vk::Rect2D rect = sceneRect(input);

    // Cleared to zero coverage: in the solid-background mode, composite.frag fills uncovered pixels.
    vk::RenderingAttachmentInfo colorAttachment{};
    colorAttachment.setImageLayout(vk::ImageLayout::eColorAttachmentOptimal)
        .setLoadOp(vk::AttachmentLoadOp::eClear)
        .setClearValue(vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f })));
    if (msaa) {
        colorAttachment.setImageView(m_msaaColor.view)
            .setStoreOp(vk::AttachmentStoreOp::eDontCare)
            .setResolveMode(vk::ResolveModeFlagBits::eAverage)
            .setResolveImageView(m_hdrColor.view)
            .setResolveImageLayout(vk::ImageLayout::eColorAttachmentOptimal);
    }
    else {
        colorAttachment.setImageView(m_hdrColor.view).setStoreOp(vk::AttachmentStoreOp::eStore);
    }

    vk::RenderingAttachmentInfo depthAttachment{};
    if (depthPrepass) {
        // Depth is final already; this pass only tests against it.
        depthAttachment.setLoadOp(vk::AttachmentLoadOp::eLoad).setStoreOp(vk::AttachmentStoreOp::eNone);
        if (msaa)
            depthAttachment.setImageView(m_msaaDepth.view).setImageLayout(vk::ImageLayout::eDepthAttachmentOptimal);
        else
            depthAttachment.setImageView(m_sceneDepth.view).setImageLayout(vk::ImageLayout::eDepthReadOnlyOptimal);
    }
    else {
        depthAttachment.setImageLayout(vk::ImageLayout::eDepthAttachmentOptimal)
            .setLoadOp(vk::AttachmentLoadOp::eClear)
            .setClearValue(vk::ClearValue(vk::ClearDepthStencilValue{ 1.0f, 0 }));
        if (msaa) {
            depthAttachment.setImageView(m_msaaDepth.view)
                .setStoreOp(vk::AttachmentStoreOp::eDontCare)
                .setResolveMode(vk::ResolveModeFlagBits::eSampleZero)
                .setResolveImageView(m_sceneDepth.view)
                .setResolveImageLayout(vk::ImageLayout::eDepthAttachmentOptimal);
        }
        else {
            depthAttachment.setImageView(m_sceneDepth.view).setStoreOp(vk::AttachmentStoreOp::eStore);
        }
    }

    vk::RenderingInfo renderInfo{};
    renderInfo.setRenderArea(rect)
        .setLayerCount(1)
        .setColorAttachments(colorAttachment)
        .setPDepthAttachment(&depthAttachment);
    beginRendering(cmd, renderInfo);

    setDefaultDrawState(cmd, m_samples, viewportFor(rect), rect);
    cmd.setDepthTestEnable(VK_TRUE);
    cmd.setDepthWriteEnable(depthPrepass ? VK_FALSE : VK_TRUE);
    cmd.setDepthCompareOp(depthPrepass ? vk::CompareOp::eLessOrEqual : vk::CompareOp::eLess);
    bindShaderPair(cmd, m_meshShaders);
    cmd.setVertexInputEXT(1, &m_meshBinding, static_cast<uint32_t>(m_meshAttributes.size()), m_meshAttributes.data());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_meshLayout, 0, 1, &m_frameSets[m_currentFrame], 0, nullptr);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_meshLayout, 4, 1, &m_lightingSet, 0, nullptr);
    recordMeshRuns(cmd, m_opaqueRuns);

    // Sky after the opaque geometry, so it is only shaded where nothing covers it.
    cmd.setDepthWriteEnable(VK_FALSE);
    cmd.setDepthCompareOp(vk::CompareOp::eLessOrEqual);
    if (m_settings.background == BackgroundMode::Realistic)
        drawFx(cmd, m_skyShaders, { m_skyLutSet });

    // Transparent-ish layers: depth-tested against the opaque scene, no depth writes.
    setAlphaBlending(cmd, true);
    if (input.showGrid)
        drawFx(cmd, m_gridShaders, {});

    if (!m_blendRuns.empty()) {
        bindShaderPair(cmd, m_meshShaders);
        cmd.setVertexInputEXT(1, &m_meshBinding, static_cast<uint32_t>(m_meshAttributes.size()), m_meshAttributes.data());
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_meshLayout, 0, 1, &m_frameSets[m_currentFrame], 0, nullptr);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_meshLayout, 4, 1, &m_lightingSet, 0, nullptr);
        recordMeshRuns(cmd, m_blendRuns);
    }

    setAlphaBlending(cmd, false);
    recordPathLines(cmd, input);
    cmd.endRendering();

    // The HDR image feeds bloom and the composite; without a prepass the depth was only written just now
    // and the selection mask samples it next. Resolves count as color attachment writes.
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    std::vector<vk::ImageMemoryBarrier2> barriers = {
        imageBarrier(vk::Image(m_hdrColor.image), vk::ImageAspectFlagBits::eColor,
            Stage::eColorAttachmentOutput, Access::eColorAttachmentWrite,
            Stage::eFragmentShader, Access::eShaderSampledRead,
            vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal),
    };
    if (!depthPrepass) {
        barriers.push_back(imageBarrier(vk::Image(m_sceneDepth.image), vk::ImageAspectFlagBits::eDepth,
            Stage::eLateFragmentTests | Stage::eColorAttachmentOutput,
            Access::eDepthStencilAttachmentWrite | Access::eColorAttachmentWrite,
            Stage::eFragmentShader, Access::eShaderSampledRead,
            vk::ImageLayout::eDepthAttachmentOptimal, vk::ImageLayout::eDepthReadOnlyOptimal));
    }
    pipelineBarriers(cmd, barriers);
}

void Renderer::recordMeshRuns(vk::CommandBuffer cmd, const std::vector<DrawRun>& runs)
{
    const GPUModel* boundModel = nullptr;
    vk::DescriptorSet boundSets[3] = { nullptr, nullptr, nullptr };
    for (const DrawRun& run : runs) {
        if (run.model != boundModel) {
            bindModelBuffers(cmd, run.model);
            boundModel = run.model;
        }
        if (run.sets[0] != boundSets[0] || run.sets[1] != boundSets[1] || run.sets[2] != boundSets[2]) {
            for (int k = 0; k < 3; ++k) boundSets[k] = run.sets[k];
            cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_meshLayout, 1, 3, boundSets, 0, nullptr);
        }
        recordRun(cmd, run);
    }
}

void Renderer::recordPathLines(vk::CommandBuffer cmd, const FrameInput& input)
{
    if (!input.showPath || m_pathLines.buffer == VK_NULL_HANDLE || m_pathLines.vertexCount == 0)
        return;

    bindShaderPair(cmd, m_gizmoShaders);
    cmd.setPrimitiveTopology(vk::PrimitiveTopology::eLineList);
    cmd.setLineWidth(m_context->features().wideLines ? 2.0f : 1.0f);
    cmd.setDepthTestEnable(VK_TRUE); // the path is hidden behind objects
    cmd.setDepthWriteEnable(VK_FALSE);
    cmd.setVertexInputEXT(1, &kLineBinding, static_cast<uint32_t>(kLineAttributes.size()), kLineAttributes.data());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_gizmoLayout, 0, 1, &m_frameSets[m_currentFrame], 0, nullptr);

    const glm::mat4 identity(1.0f);
    cmd.pushConstants(m_gizmoLayout, vk::ShaderStageFlagBits::eVertex, 0, sizeof(glm::mat4), &identity);
    const vk::Buffer buffer(m_pathLines.buffer);
    const vk::DeviceSize offset = 0;
    const vk::DeviceSize size = sizeof(GizmoVertex) * m_pathLines.vertexCount;
    const vk::DeviceSize stride = sizeof(GizmoVertex);
    cmd.bindVertexBuffers2(0, 1, &buffer, &offset, &size, &stride);
    cmd.draw(m_pathLines.vertexCount, 1, 0, 0);
    cmd.setPrimitiveTopology(vk::PrimitiveTopology::eTriangleList);
}

// R = silhouette of the highlighted geometry regardless of depth, G = where it is visible in the scene.
void Renderer::recordSelectionMask(vk::CommandBuffer cmd, const FrameInput& input)
{
    const vk::Image maskImage(m_selectionMask.image);
    const auto colorAspect = vk::ImageAspectFlagBits::eColor;

    const vk::ImageMemoryBarrier2 toAttachment = imageBarrier(maskImage, colorAspect,
        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eNone,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);
    pipelineBarriers(cmd, { &toAttachment, 1 });

    vk::RenderingAttachmentInfo maskAttachment{};
    maskAttachment.setImageView(m_selectionMask.view)
        .setImageLayout(vk::ImageLayout::eColorAttachmentOptimal)
        .setLoadOp(vk::AttachmentLoadOp::eClear)
        .setStoreOp(vk::AttachmentStoreOp::eStore)
        .setClearValue(vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 0.0f })));
    vk::RenderingInfo renderInfo{};
    renderInfo.setRenderArea({ {0, 0}, m_swapchain.extent() })
        .setLayerCount(1)
        .setColorAttachments(maskAttachment);
    beginRendering(cmd, renderInfo);

    const vk::Rect2D rect = sceneRect(input);
    setDefaultDrawState(cmd, vk::SampleCountFlagBits::e1, viewportFor(rect), rect);
    cmd.setColorWriteMaskEXT(0, vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG);
    cmd.setColorBlendEnableEXT(0, VK_TRUE);
    vk::ColorBlendEquationEXT maxEquation{};
    maxEquation.srcColorBlendFactor = vk::BlendFactor::eOne;
    maxEquation.dstColorBlendFactor = vk::BlendFactor::eOne;
    maxEquation.colorBlendOp = vk::BlendOp::eMax;
    maxEquation.srcAlphaBlendFactor = vk::BlendFactor::eOne;
    maxEquation.dstAlphaBlendFactor = vk::BlendFactor::eOne;
    maxEquation.alphaBlendOp = vk::BlendOp::eMax;
    cmd.setColorBlendEquationEXT(0, 1, &maxEquation);

    bindShaderPair(cmd, m_maskShaders);
    cmd.setVertexInputEXT(1, &m_meshBinding, static_cast<uint32_t>(m_meshAttributes.size()), m_meshAttributes.data());
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_fxLayout, 0, 1, &m_frameSets[m_currentFrame], 0, nullptr);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_fxLayout, 1, 1, &m_sceneDepthSet, 0, nullptr);
    const std::array<std::byte, kFxPushConstantSize> noPush{};
    cmd.pushConstants(m_fxLayout, vk::ShaderStageFlagBits::eFragment, 0, kFxPushConstantSize, noPush.data());

    const GPUModel* boundModel = nullptr;
    for (const DrawRun& run : m_highlightRuns) {
        if (run.model != boundModel) {
            bindModelBuffers(cmd, run.model);
            boundModel = run.model;
        }
        recordRun(cmd, run);
    }
    cmd.endRendering();

    const vk::ImageMemoryBarrier2 toSampled = imageBarrier(maskImage, colorAspect,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
        vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal);
    pipelineBarriers(cmd, { &toSampled, 1 });
}

// Single-sample pass on the swapchain image: the tone-mapped scene (or its FXAA'd copy), the selection
// outline, then ImGui.
void Renderer::recordFinalPass(vk::CommandBuffer cmd, uint32_t imageIndex, const FrameInput& input, bool drawOutline)
{
    const vk::Rect2D rect = sceneRect(input);
    const vk::Extent2D extent = m_swapchain.extent();
    const CompositePushConstants composite = compositeConstants();
    if (m_settings.fxaa) {
        beginFxPass(cmd, m_ldrColor.view, rect, vk::AttachmentLoadOp::eDontCare);
        drawFx(cmd, m_compositeShaders, { m_hdrSet, m_bloomSets[0] }, &composite, sizeof(composite));
        cmd.endRendering();
        colorTargetToSampled(cmd, m_ldrColor);
    }

    const vk::Image swapImage(m_swapchain.images()[imageIndex]);
    const vk::ImageMemoryBarrier2 toAttachment = imageBarrier(swapImage, vk::ImageAspectFlagBits::eColor,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eNone,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal);
    pipelineBarriers(cmd, { &toAttachment, 1 });

    // Outside the scene viewport the UI covers everything, so a plain clear is enough there.
    vk::RenderingAttachmentInfo colorAttachment{};
    colorAttachment.setImageView(m_swapchain.imageViews()[imageIndex])
        .setImageLayout(vk::ImageLayout::eColorAttachmentOptimal)
        .setLoadOp(vk::AttachmentLoadOp::eClear)
        .setStoreOp(vk::AttachmentStoreOp::eStore)
        .setClearValue(vk::ClearValue(vk::ClearColorValue(std::array<float, 4>{ 0.1f, 0.1f, 0.1f, 1.0f })));
    vk::RenderingInfo renderInfo{};
    renderInfo.setRenderArea({ {0, 0}, extent })
        .setLayerCount(1)
        .setColorAttachments(colorAttachment);
    beginRendering(cmd, renderInfo);
    setDefaultDrawState(cmd, vk::SampleCountFlagBits::e1, viewportFor(rect), rect);

    if (m_settings.fxaa) {
        FxaaPushConstants push{};
        push.texel = glm::vec4(1.0f / extent.width, 1.0f / extent.height, 0.0f, 0.0f);
        push.uvClamp = glm::vec4((rect.offset.x + 0.5f) / extent.width, (rect.offset.y + 0.5f) / extent.height,
            (rect.offset.x + rect.extent.width - 0.5f) / extent.width,
            (rect.offset.y + rect.extent.height - 0.5f) / extent.height);
        drawFx(cmd, m_fxaaShaders, { m_ldrSet }, &push, sizeof(push));
    }
    else {
        drawFx(cmd, m_compositeShaders, { m_hdrSet, m_bloomSets[0] }, &composite, sizeof(composite));
    }

    if (drawOutline) {
        setAlphaBlending(cmd, true);
        OutlinePushConstants push{};
        push.color = glm::vec4(kOutlineColor, 1.0f);
        push.occludedAlpha = kOutlineOccludedAlpha;
        push.widthPixels = kOutlineWidthPixels;
        drawFx(cmd, m_outlineShaders, { m_selectionMaskSet }, &push, sizeof(push));
        setAlphaBlending(cmd, false);
    }

    if (input.imgui && input.imgui->TotalVtxCount > 0)
        ImGui_ImplVulkan_RenderDrawData(input.imgui, cmd);
    cmd.endRendering();

    const vk::ImageMemoryBarrier2 toPresent = imageBarrier(swapImage, vk::ImageAspectFlagBits::eColor,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::PipelineStageFlagBits2::eBottomOfPipe, vk::AccessFlagBits2::eNone,
        vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::ePresentSrcKHR);
    pipelineBarriers(cmd, { &toPresent, 1 });
}

Renderer::FrameStatus Renderer::submitAndPresent(vk::CommandBuffer cmd, uint32_t imageIndex)
{
    const vk::CommandBufferSubmitInfo cmdInfo{ cmd };
    const vk::SemaphoreSubmitInfo waitInfo{
        m_imageAvailableSemaphores[m_currentFrame], 0, vk::PipelineStageFlagBits2::eColorAttachmentOutput };
    const vk::SemaphoreSubmitInfo signalInfo{
        m_renderFinishedSemaphores[imageIndex], 0, vk::PipelineStageFlagBits2::eAllCommands };

    vk::SubmitInfo2 submit{};
    submit.setCommandBufferInfos(cmdInfo)
        .setWaitSemaphoreInfos(waitInfo)
        .setSignalSemaphoreInfos(signalInfo);
    (void)m_context->graphicsQueue().submit2(submit, m_inFlightFences[m_currentFrame]);

    const vk::SwapchainKHR swapchain = m_swapchain.handle();
    vk::PresentInfoKHR present{};
    present.setWaitSemaphores(m_renderFinishedSemaphores[imageIndex])
        .setSwapchains(swapchain)
        .setImageIndices(imageIndex);

    // Pointer overload: returns the raw result instead of asserting on eErrorOutOfDateKHR.
    const vk::Result presentResult = m_context->presentQueue().presentKHR(&present);
    if (presentResult == vk::Result::eErrorOutOfDateKHR || presentResult == vk::Result::eSuboptimalKHR)
        m_swapchainDirty = true;
    else if (presentResult != vk::Result::eSuccess)
        LOG_ERROR("Failed to present\n");

    m_currentFrame = (m_currentFrame + 1) % m_framesInFlight;
    return FrameStatus::Rendered;
}
