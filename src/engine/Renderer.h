#pragma once
#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <unordered_map>
#include <vector>
#include "Buffers.h"
#include "Camera.h"
#include "Gizmo.h"
#include "GraphicsSettings.h"
#include "RenderTypes.h"
#include "ShaderUtils.h"
#include "Shadow.h"
#include "Swapchain.h"

struct SDL_Window;
struct ImDrawData;
class VulkanContext;
class ModelManager;
struct GPUModel;
struct ModelInstance;
struct SubmeshInfo;
struct Material;

// Rectangle in window coordinates (not pixels).
struct ViewRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

// Geometry drawn with the selection outline.
struct SelectionHighlight {
    int instance = -1;
    bool wholeInstance = true;
    // Submesh indices of `instance`; used only when wholeInstance is false.
    std::vector<uint32_t> submeshes;
};

struct FrameInput {
    ModelManager* models = nullptr;
    // Computed by the mode; the projection must come from getProjection() so picking matches.
    glm::mat4 view{ 1.0f };
    glm::mat4 proj{ 1.0f };
    glm::vec3 cameraPosition{ 0.0f };
    // The scene is drawn only into this part of the window; UI covers the rest.
    ViewRect viewport;
    float windowWidth = 1.0f;
    float windowHeight = 1.0f;
    SelectionHighlight highlight;
    bool showPath = false;
    bool showGrid = false;
    ImDrawData* imgui = nullptr;
    float time = 0.0f;
};

// Owns every GPU resource used to draw a frame and records/submits/presents it. The sun, sky and
// post-processing come from GraphicsSettings.
// Requires an ImGui context to exist before init() (it initializes the ImGui Vulkan backend).
//
// Frame: sky LUT (when the sun changes) -> shadow cascades (those that changed) -> depth prepass + half-res
// AO / contact shadows (when enabled) -> HDR scene pass -> selection mask -> bloom -> composite (tone map)
// [-> FXAA] -> outline + ImGui on the swapchain image.
class Renderer {
public:
    enum class FrameStatus {
        Rendered,
        Skipped, // swapchain out of date; it is rebuilt on the next prepareSwapchain()
        Failed,  // unrecoverable acquire error
    };

    Renderer() = default;
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool init(VulkanContext& context, SDL_Window* window, const GraphicsSettings& settings);
    void shutdown();

    // Applies changed settings: VSync rebuilds the swapchain on the next prepareSwapchain(); MSAA and
    // shadow map size/cascades recreate their resources right away (waits for the GPU to go idle).
    void applySettings(const GraphicsSettings& settings);
    const GraphicsSettings& settings() const { return m_settings; }
    const RenderCapabilities& capabilities() const { return m_capabilities; }

    // Resources ModelManager needs to upload textures and allocate their descriptor sets.
    vk::CommandPool commandPool() const { return m_commandPool; }
    vk::DescriptorPool descriptorPool() const { return m_descriptorPool; }
    vk::DescriptorSetLayout textureSetLayout() const { return m_textureSetLayout; }
    vk::Sampler textureSampler() const { return m_textureSampler; }

    // Replaces the camera-path line list (pairs of vertices). An empty list hides the path.
    void setPathLines(const std::vector<GizmoVertex>& vertices);

    void requestSwapchainRebuild() { m_swapchainDirty = true; }
    // Rebuilds the swapchain if a rebuild was requested. False while there is nothing to render into
    // (window minimized / zero-sized) or recreation failed; skip the frame then.
    bool prepareSwapchain();

    uint32_t framebufferWidth() const { return m_swapchain.extent().width; }
    uint32_t framebufferHeight() const { return m_swapchain.extent().height; }

    FrameStatus renderFrame(const FrameInput& input);

private:
    struct FrameDrawBuffers {
        HostBuffer draws;
        HostBuffer transforms;
        HostBuffer indirect;
    };

    struct LineBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        uint32_t vertexCount = 0;
    };

    struct InstanceRenderData {
        const ModelInstance* instance;
        glm::mat4 transform;
        const uint8_t* submeshFlags;
        uint32_t transformIndex;
    };

    struct RenderBatch {
        GPUModel* model = nullptr;
        std::vector<InstanceRenderData> instances;
    };

    // Visible models this frame, keyed by model index.
    struct FrameBatches {
        std::unordered_map<size_t, RenderBatch> main;
        std::unordered_map<size_t, RenderBatch> shadow; // casters in any cascade
    };

    struct CullResult {
        bool visibleMain = false;
        uint8_t shadowMask = 0; // bit c: the instance casts into cascade c
        float maxScale = 1.0f;
        glm::mat4 transform = glm::mat4(1.0f);
        const ModelInstance* instance = nullptr;
        size_t modelIndex = 0;
        GPUModel* gpuModel = nullptr;
        FrustumPlanes mainPlanes{};
        std::array<FrustumPlanes, kMaxShadowCascades> shadowPlanes{};
        std::vector<uint8_t> submeshFlags;
    };

    struct RenderImage {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation allocation = VK_NULL_HANDLE;
        vk::ImageView view; // every mip level
    };

    struct CullChunk {
        size_t instanceIdx;
        size_t begin;
        size_t end;
    };

    // Consecutive indirect commands that share model buffers, texture sets and blend state.
    struct DrawRun {
        GPUModel* model;
        vk::DescriptorSet sets[3]; // null = "any set works" (only used by the shadow pass)
        bool blend;
        uint32_t firstCommand;
        uint32_t commandCount;
    };

    struct SunState {
        glm::vec3 direction{ 0.0f, -1.0f, 0.0f }; // direction the light travels
        glm::vec3 topIrradiance{ 0.0f };          // above the atmosphere; lights the sky
        glm::vec3 groundIrradiance{ 0.0f };       // after the atmosphere; zero while the sun is off
    };

    void queryCapabilities();
    bool createFrameResources();
    bool createDescriptors();
    bool createShaders();
    bool initImGuiBackend();
    bool createShadowMapResources();
    bool createRenderTargets();
    void destroyRenderTargets();
    bool createSkyResources();
    void destroySkyResources();
    // extent {0, 0} = swapchain size. The view covers every mip level.
    bool createRenderImage(vk::Format format, vk::ImageUsageFlags usage, vk::SampleCountFlagBits samples,
        vk::ImageAspectFlags aspect, RenderImage& out, vk::Extent2D extent = {}, uint32_t mipLevels = 1);
    bool createMipView(const RenderImage& image, vk::Format format, uint32_t mip, vk::ImageView& out);
    void destroyRenderImage(RenderImage& image);
    bool recreateSwapchain();
    void writeDrawDescriptors(uint32_t frame);
    // Points every image descriptor set at the current render targets, shadow map and sky textures.
    void writeImageDescriptors();
    vk::SampleCountFlagBits effectiveSampleCount() const;

    bool createLineBuffer(const std::vector<GizmoVertex>& vertices, LineBuffer& out);
    void destroyLineBuffer(LineBuffer& buffer);

    void updateSun();
    bool shadowsActive() const;
    bool contactShadowsActive() const;
    bool depthPrepassEnabled() const;
    uint64_t skyHash() const;

    FrameUBO buildFrameUBO(const FrameInput& input) const;
    // Also returns, per cascade, the hash of everything that affects its shadow map layer.
    void cullAndBatch(const FrameInput& input, const glm::mat4& viewProj, FrameBatches& batches,
        std::array<uint64_t, kMaxShadowCascades>& shadowHashes);
    void buildDrawStreams(const FrameInput& input, FrameBatches& batches);
    void buildHighlightStream(const FrameInput& input);
    void appendDraw(std::vector<DrawRun>& runs, GPUModel* model, const vk::DescriptorSet sets[3], bool blend,
        const SubmeshInfo& sub, uint32_t transformIndex, const glm::vec3& tint);
    uint32_t pushDrawData(const SubmeshInfo& sub, uint32_t transformIndex, const glm::vec3& tint);
    uint32_t pushTransform(const glm::mat4& transform);
    void materialSets(const ModelManager& models, const GPUModel* model, const Material& material,
        vk::DescriptorSet out[3]) const;
    void uploadDrawStreams();

    // Sets every piece of dynamic state the shader-object draws rely on to a known default.
    void setDefaultDrawState(vk::CommandBuffer cmd, vk::SampleCountFlagBits samples,
        const vk::Viewport& viewport, const vk::Rect2D& scissor) const;
    void setAlphaBlending(vk::CommandBuffer cmd, bool enabled) const;
    void setAdditiveBlending(vk::CommandBuffer cmd) const;
    vk::Rect2D sceneRect(const FrameInput& input) const;
    void bindModelBuffers(vk::CommandBuffer cmd, const GPUModel* model) const;
    void recordRun(vk::CommandBuffer cmd, const DrawRun& run) const;
    void recordDepthRuns(vk::CommandBuffer cmd, vk::PipelineLayout layout, const std::vector<DrawRun>& runs,
        const ModelManager& models) const;
    void recordShadowPasses(vk::CommandBuffer cmd, const ModelManager& models);
    // Moves this frame's render targets into the layout of their first use; their old contents are discarded.
    void transitionFrameTargets(vk::CommandBuffer cmd, bool depthPrepass);
    void recordDepthPrepass(vk::CommandBuffer cmd, const FrameInput& input);
    void recordScenePass(vk::CommandBuffer cmd, const FrameInput& input, bool depthPrepass);
    void recordMeshRuns(vk::CommandBuffer cmd, const std::vector<DrawRun>& runs);
    void recordPathLines(vk::CommandBuffer cmd, const FrameInput& input);
    void recordSelectionMask(vk::CommandBuffer cmd, const FrameInput& input);
    void recordFinalPass(vk::CommandBuffer cmd, uint32_t imageIndex, const FrameInput& input, bool drawOutline);
    FrameStatus submitAndPresent(vk::CommandBuffer cmd, uint32_t imageIndex);

    // ---- RendererEffects.cpp: fullscreen passes ----
    // Begins dynamic rendering into one color attachment and sets the default draw state for `area`.
    void beginFxPass(vk::CommandBuffer cmd, vk::ImageView target, const vk::Rect2D& area, vk::AttachmentLoadOp loadOp) const;
    // Draws a fullscreen triangle; `inputs` are bound as sets 1.. after the frame set.
    void drawFx(vk::CommandBuffer cmd, const ShaderPair& shaders, std::initializer_list<vk::DescriptorSet> inputs,
        const void* pushData = nullptr, uint32_t pushSize = 0) const;
    void colorTargetToSampled(vk::CommandBuffer cmd, const RenderImage& image, uint32_t mip = 0) const;
    void recordSkyPasses(vk::CommandBuffer cmd);
    void recordAoPasses(vk::CommandBuffer cmd, const FrameInput& input);
    void recordBloom(vk::CommandBuffer cmd, const FrameInput& input);
    CompositePushConstants compositeConstants() const;

    VulkanContext* m_context = nullptr;
    SDL_Window* m_window = nullptr;
    vk::Device m_device;
    VmaAllocator m_allocator = VK_NULL_HANDLE;

    Swapchain m_swapchain;
    bool m_swapchainDirty = false;
    uint32_t m_framesInFlight = 0;
    uint32_t m_currentFrame = 0;
    uint32_t m_frameIndex = 0;

    GraphicsSettings m_settings;
    RenderCapabilities m_capabilities;
    SunState m_sun;

    static constexpr vk::Format kDepthFormat = vk::Format::eD32Sfloat;
    static constexpr vk::Format kSelectionMaskFormat = vk::Format::eR8G8Unorm;
    static constexpr vk::Format kHdrFormat = vk::Format::eR16G16B16A16Sfloat;
    static constexpr vk::Format kLdrFormat = vk::Format::eR8G8B8A8Srgb;
    static constexpr vk::Format kAoFormat = vk::Format::eR8G8Unorm;
    static constexpr vk::Format kAoDepthFormat = vk::Format::eR32Sfloat;
    static constexpr uint32_t kMaxBloomMips = 6;
    static constexpr vk::Extent2D kSkyLutExtent{ 256, 128 };
    static constexpr uint32_t kSkyLutMips = 8;
    static constexpr vk::Extent2D kSkyIrradianceExtent{ 32, 16 };

    // Swapchain-sized targets, rebuilt on resize and MSAA changes. With MSAA the multisampled targets
    // resolve into hdrColor / sceneDepth.
    vk::SampleCountFlagBits m_samples = vk::SampleCountFlagBits::e1;
    RenderImage m_sceneDepth;
    RenderImage m_msaaColor;
    RenderImage m_msaaDepth;
    RenderImage m_selectionMask;
    RenderImage m_hdrColor;  // linear HDR scene, alpha = coverage (see composite.frag)
    RenderImage m_ldrColor;  // tone-mapped scene, FXAA input
    vk::Extent2D m_halfExtent{};
    RenderImage m_aoDepth;   // half resolution view depth
    RenderImage m_aoRaw;     // half resolution AO + contact shadow; holds the blurred result at the end
    RenderImage m_aoTemp;
    RenderImage m_bloom;     // half resolution mip chain
    std::array<vk::ImageView, kMaxBloomMips> m_bloomMipViews{};
    uint32_t m_bloomMips = 0;

    // Sky radiance by direction (see sky.glsl) and the diffuse irradiance derived from it.
    RenderImage m_skyLut;
    vk::ImageView m_skyLutTargetView; // mip 0 only
    RenderImage m_skyIrradiance;
    uint64_t m_skyHash = 0;
    bool m_skyValid = false;

    vk::CommandPool m_commandPool;
    std::vector<vk::CommandBuffer> m_commandBuffers;
    std::vector<vk::Semaphore> m_imageAvailableSemaphores;
    // Indexed by swapchain image, not by frame in flight.
    std::vector<vk::Semaphore> m_renderFinishedSemaphores;
    std::vector<vk::Fence> m_inFlightFences;
    std::vector<UBOBuffer> m_frameUBOs;

    vk::Sampler m_textureSampler;
    vk::Sampler m_nearestSampler;
    vk::Sampler m_linearClampSampler;
    vk::Sampler m_skySampler; // wraps around horizontally, clamps vertically
    vk::DescriptorSetLayout m_textureSetLayout;
    vk::DescriptorSetLayout m_frameSetLayout;
    vk::DescriptorSetLayout m_lightingSetLayout;
    vk::DescriptorPool m_descriptorPool;
    std::vector<vk::DescriptorSet> m_frameSets;
    std::vector<std::unique_ptr<FrameDrawBuffers>> m_frameDrawBuffers;
    uint32_t m_maxDrawIndirectCount = 0;

    ShadowMapResources m_shadowMap;
    std::array<ShadowCascade, kMaxShadowCascades> m_cascades{};
    std::array<uint64_t, kMaxShadowCascades> m_cascadeHashes{};
    std::array<bool, kMaxShadowCascades> m_renderCascade{};
    bool m_shadowMapValid = false;

    // Set 4 of the mesh shaders: shadow map, AO and sky textures.
    vk::DescriptorSet m_lightingSet;
    // Single-image sets (m_textureSetLayout) for the effect passes.
    vk::DescriptorSet m_sceneDepthSet;
    vk::DescriptorSet m_selectionMaskSet;
    vk::DescriptorSet m_hdrSet;
    vk::DescriptorSet m_ldrSet;
    vk::DescriptorSet m_aoDepthSet;
    vk::DescriptorSet m_aoRawSet;
    vk::DescriptorSet m_aoTempSet;
    vk::DescriptorSet m_skyLutSet;
    std::array<vk::DescriptorSet, kMaxBloomMips> m_bloomSets{};

    ShaderPair m_meshShaders;
    ShaderPair m_prepassShaders;
    ShaderPair m_shadowShaders;
    ShaderPair m_gizmoShaders;
    ShaderPair m_skyShaders;
    ShaderPair m_gridShaders;
    ShaderPair m_maskShaders;
    ShaderPair m_outlineShaders;
    ShaderPair m_aoDepthShaders;
    ShaderPair m_aoShaders;
    ShaderPair m_aoBlurShaders;
    ShaderPair m_bloomDownShaders;
    ShaderPair m_bloomUpShaders;
    ShaderPair m_compositeShaders;
    ShaderPair m_fxaaShaders;
    ShaderPair m_skyLutShaders;
    ShaderPair m_skyIrradianceShaders;
    vk::PipelineLayout m_meshLayout;
    vk::PipelineLayout m_prepassLayout;
    vk::PipelineLayout m_shadowLayout;
    vk::PipelineLayout m_gizmoLayout;
    // Every fullscreen effect and the selection mask: set 0 = frame data, sets 1-3 = sampled images.
    vk::PipelineLayout m_fxLayout;
    vk::VertexInputBindingDescription2EXT m_meshBinding;
    std::array<vk::VertexInputAttributeDescription2EXT, 4> m_meshAttributes;

    LineBuffer m_pathLines;

    vk::DescriptorPool m_imguiDescriptorPool;
    bool m_imguiInitialized = false;

    // Per-frame scratch kept as members so their capacity survives between frames.
    std::vector<CullResult> m_cullResults;
    std::vector<size_t> m_cullIndices;
    std::vector<CullChunk> m_cullChunks;
    std::vector<GpuTransform> m_frameTransforms;
    std::vector<GpuDrawData> m_frameDraws;
    std::vector<vk::DrawIndexedIndirectCommand> m_frameCommands;
    std::array<std::vector<DrawRun>, kMaxShadowCascades> m_shadowRuns;
    std::vector<DrawRun> m_opaqueRuns;
    std::vector<DrawRun> m_blendRuns;
    std::vector<DrawRun> m_highlightRuns;
};
