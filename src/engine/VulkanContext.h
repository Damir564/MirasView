#pragma once
#include <vulkan/vulkan.hpp>
#include <VkBootstrap.h>
#include <vk_mem_alloc.h>
#include <cstdint>

struct SDL_Window;
struct SDL_SharedObject;

// Where the device gets the features the renderer needs, fastest first. init() tries them in this order.
enum class VulkanBackend {
    Native,   // the GPU driver supports everything itself
    Emulated, // the bundled Khronos layers emulate VK_EXT_shader_object / VK_KHR_synchronization2 on the GPU
    Software, // the bundled Mesa lavapipe driver renders on the CPU
};

const char* vulkanBackendName(VulkanBackend backend);

// Features the renderer uses when the device has them and works around otherwise.
struct OptionalDeviceFeatures {
    bool wideLines = false;
    bool depthClamp = false;
    bool samplerAnisotropy = false;
    bool multiDrawIndirect = false;
};

// Owns the Vulkan instance, surface, device, queues and VMA allocator.
class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext();

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    // Loads the system Vulkan loader, or the one bundled in vulkan/ when there is none or it is too old.
    // Must run after SDL_Init and before the SDL_WINDOW_VULKAN window is created: SDL keeps using this loader.
    bool loadLibrary();
    // Tries the backends from firstBackend on. Starting at Emulated keeps the shader object layer active even
    // where the driver has the extension. Logs the reason and releases anything partially created on failure.
    bool init(SDL_Window* window, bool enableValidation, VulkanBackend firstBackend = VulkanBackend::Native);
    void shutdown();

    const vkb::Instance& vkbInstance() const { return m_vkbInstance; }
    vk::Instance instance() const { return m_instance; }
    vk::SurfaceKHR surface() const { return m_surface; }
    const vkb::PhysicalDevice& vkbPhysicalDevice() const { return m_vkbPhysicalDevice; }
    vk::PhysicalDevice physicalDevice() const { return m_physicalDevice; }
    const vkb::Device& vkbDevice() const { return m_vkbDevice; }
    vk::Device device() const { return m_device; }
    VmaAllocator allocator() const { return m_allocator; }
    vk::Queue graphicsQueue() const { return m_graphicsQueue; }
    vk::Queue presentQueue() const { return m_presentQueue; }
    uint32_t graphicsQueueFamily() const { return m_graphicsQueueFamily; }
    VulkanBackend backend() const { return m_backend; }
    // The Vulkan version used with the device: its own version, capped to what the engine targets.
    uint32_t apiVersion() const { return m_apiVersion; }
    const OptionalDeviceFeatures& features() const { return m_features; }

private:
    bool tryBackend(SDL_Window* window, bool enableValidation, VulkanBackend backend, bool forceEmulation);
    bool createInstance(bool enableValidation, VulkanBackend backend, bool forceEmulation);
    bool loadSoftwareDriver();
    bool createSurface(SDL_Window* window);
    bool createDevice();
    bool createAllocator();
    void destroyDevice();
    void destroyInstance();

    bool m_libraryLoaded = false;
    SDL_SharedObject* m_softwareDriver = nullptr;
    PFN_vkGetInstanceProcAddr m_softwareGetInstanceProcAddr = nullptr;

    vkb::Instance m_vkbInstance;
    vk::Instance m_instance;
    vk::SurfaceKHR m_surface;
    vkb::PhysicalDevice m_vkbPhysicalDevice;
    vk::PhysicalDevice m_physicalDevice;
    vkb::Device m_vkbDevice;
    vk::Device m_device;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    vk::Queue m_graphicsQueue;
    vk::Queue m_presentQueue;
    uint32_t m_graphicsQueueFamily = 0;
    VulkanBackend m_backend = VulkanBackend::Native;
    uint32_t m_apiVersion = VK_API_VERSION_1_2;
    OptionalDeviceFeatures m_features;
};
