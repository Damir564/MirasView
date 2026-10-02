#define VKB_DISABLE_DEBUG_BREAK
#include "VulkanContext.h"
#include <volk.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include "Log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace {
// 1.2 devices work too when they have the 1.3 features the renderer uses as extensions.
constexpr uint32_t kTargetApiVersion = VK_API_VERSION_1_3;
// The oldest loader with VK_LUNARG_direct_driver_loading and VK_ADD_LAYER_PATH, which the fallbacks rely on.
constexpr uint32_t kMinLoaderVersion = VK_MAKE_API_VERSION(0, 1, 3, 238);

constexpr const char* kShaderObjectLayer = "VK_LAYER_KHRONOS_shader_object";
constexpr const char* kSynchronization2Layer = "VK_LAYER_KHRONOS_synchronization2";

// The Vulkan runtime shipped next to the executable (see cmake/Packaging.cmake).
std::filesystem::path runtimePath(const char* file = nullptr)
{
    std::filesystem::path path;
    if (const char* base = SDL_GetBasePath())
        path = std::filesystem::path(reinterpret_cast<const char8_t*>(base));
    path /= "vulkan";
    if (file)
        path /= file;
    return path;
}

std::string toUtf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return std::string(text.begin(), text.end());
}

// vk-bootstrap cannot extend VkInstanceCreateInfo, so its vkCreateInstance goes through this hook, which
// adds the software driver while that backend is being created.
PFN_vkGetInstanceProcAddr g_loaderGetInstanceProcAddr = nullptr;
PFN_vkCreateInstance g_loaderCreateInstance = nullptr;
const VkDirectDriverLoadingListLUNARG* g_directDrivers = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL createInstanceHook(const VkInstanceCreateInfo* createInfo,
    const VkAllocationCallbacks* allocator, VkInstance* instance)
{
    if (!g_directDrivers)
        return g_loaderCreateInstance(createInfo, allocator, instance);
    VkDirectDriverLoadingListLUNARG drivers = *g_directDrivers;
    drivers.pNext = createInfo->pNext;
    VkInstanceCreateInfo info = *createInfo;
    info.pNext = &drivers;
    return g_loaderCreateInstance(&info, allocator, instance);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL getInstanceProcAddrHook(VkInstance instance, const char* name)
{
    if (instance == VK_NULL_HANDLE && std::strcmp(name, "vkCreateInstance") == 0)
        return reinterpret_cast<PFN_vkVoidFunction>(&createInstanceHook);
    return g_loaderGetInstanceProcAddr(instance, name);
}

uint32_t loaderVersion()
{
    auto sdlGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    auto enumerateVersion = sdlGetInstanceProcAddr ? reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
        sdlGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion")) : nullptr;
    uint32_t version = VK_API_VERSION_1_0;
    if (enumerateVersion && enumerateVersion(&version) != VK_SUCCESS)
        version = VK_API_VERSION_1_0;
    return version;
}

// Lets the loader find the bundled emulation layers, keeping any paths the user added.
void addBundledLayerPath()
{
    std::wstring value = runtimePath().wstring();
    const DWORD size = GetEnvironmentVariableW(L"VK_ADD_LAYER_PATH", nullptr, 0);
    if (size > 0) {
        std::wstring existing(size, L'\0');
        existing.resize(GetEnvironmentVariableW(L"VK_ADD_LAYER_PATH", existing.data(), size));
        value += L';' + existing;
    }
    SetEnvironmentVariableW(L"VK_ADD_LAYER_PATH", value.c_str());
}
}

const char* vulkanBackendName(VulkanBackend backend)
{
    switch (backend) {
    case VulkanBackend::Native: return "Native";
    case VulkanBackend::Emulated: return "Emulated";
    case VulkanBackend::Software: return "Software (CPU)";
    }
    return "Unknown";
}

VulkanContext::~VulkanContext()
{
    shutdown();
}

bool VulkanContext::loadLibrary()
{
    // The system loader comes with the GPU driver: there is none without a Vulkan driver, and old ones lack
    // what the fallback backends need. The bundled copy covers both cases.
    const std::filesystem::path bundledLoader = runtimePath("vulkan-1.dll");
    if (SDL_Vulkan_LoadLibrary(nullptr)) {
        std::error_code ec;
        if (loaderVersion() >= kMinLoaderVersion || !std::filesystem::exists(bundledLoader, ec))
            m_libraryLoaded = true;
        else
            SDL_Vulkan_UnloadLibrary();
    }
    if (!m_libraryLoaded) {
        if (!SDL_Vulkan_LoadLibrary(toUtf8(bundledLoader).c_str())) {
            LOG_ERROR("No usable Vulkan loader: " << SDL_GetError() << "\n");
            return false;
        }
        m_libraryLoaded = true;
        LOG_INFO("Using the bundled Vulkan loader\n");
    }

    g_loaderGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    g_loaderCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        g_loaderGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    volkInitializeCustom(getInstanceProcAddrHook);
    vk::detail::defaultDispatchLoaderDynamic.init(vkGetInstanceProcAddr);
    addBundledLayerPath();
    return true;
}

bool VulkanContext::init(SDL_Window* window, bool enableValidation, VulkanBackend firstBackend)
{
    if (!m_libraryLoaded) {
        LOG_ERROR("VulkanContext::init called before loadLibrary\n");
        return false;
    }

    bool created = false;
    for (VulkanBackend backend : { VulkanBackend::Native, VulkanBackend::Emulated, VulkanBackend::Software }) {
        if (backend < firstBackend)
            continue;
        const bool forceEmulation = backend == VulkanBackend::Emulated && firstBackend == VulkanBackend::Emulated;
        if (tryBackend(window, enableValidation, backend, forceEmulation)) {
            created = true;
            break;
        }
        destroyDevice();
        destroyInstance();
    }
    if (!created || !createAllocator()) {
        shutdown();
        return false;
    }

    LOG_INFO("Vulkan device: " << m_vkbPhysicalDevice.properties.deviceName << ", Vulkan "
        << VK_API_VERSION_MAJOR(m_apiVersion) << "." << VK_API_VERSION_MINOR(m_apiVersion) << ", "
        << vulkanBackendName(m_backend) << " backend\n");
    return true;
}

bool VulkanContext::tryBackend(SDL_Window* window, bool enableValidation, VulkanBackend backend, bool forceEmulation)
{
    LOG_INFO("Trying the " << vulkanBackendName(backend) << " Vulkan backend\n");
    if (!createInstance(enableValidation, backend, forceEmulation) || !createSurface(window) || !createDevice())
        return false;
    m_backend = backend;
    return true;
}

bool VulkanContext::createInstance(bool enableValidation, VulkanBackend backend, bool forceEmulation)
{
    Uint32 extensionCount = 0;
    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    if (sdlExtensions == nullptr) {
        LOG_ERROR("SDL_Vulkan_GetInstanceExtensions failed\n");
        return false;
    }
    std::vector<const char*> extensions(sdlExtensions, sdlExtensions + extensionCount);

    auto systemInfo = vkb::SystemInfo::get_system_info(getInstanceProcAddrHook);
    if (!systemInfo) {
        LOG_ERROR("Failed to query the Vulkan loader: " << systemInfo.error().message() << "\n");
        return false;
    }

    vkb::InstanceBuilder builder(getInstanceProcAddrHook);
    builder
        .set_app_name("MirasEngine")
        .set_engine_name("MirasEngine")
        .require_api_version(kTargetApiVersion)
        .enable_extensions(extensions);

    // Referenced by the layer settings until build().
    static const VkBool32 kTrue = VK_TRUE;
    if (backend == VulkanBackend::Emulated) {
        bool anyLayer = false;
        for (const char* layer : { kShaderObjectLayer, kSynchronization2Layer }) {
            if (!systemInfo->is_layer_available(layer))
                continue;
            builder.enable_layer(layer);
            anyLayer = true;
        }
        // Without this the layer steps aside wherever the driver has the extension itself. The
        // synchronization2 layer is never forced: every 1.3 driver has it, and on 1.3 devices the core
        // vkCmdPipelineBarrier2 would bypass the layer, which only intercepts the KHR names.
        if (forceEmulation && systemInfo->is_layer_available(kShaderObjectLayer))
            builder.add_layer_setting({ kShaderObjectLayer, "force_enable", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &kTrue });
        if (!anyLayer) {
            LOG_ERROR("The Vulkan emulation layers were not found in " << toUtf8(runtimePath()) << "\n");
            return false;
        }
    }

    VkDirectDriverLoadingInfoLUNARG driverInfo{ VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG };
    VkDirectDriverLoadingListLUNARG driverList{ VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG };
    if (backend == VulkanBackend::Software) {
        if (!systemInfo->is_extension_available(VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME)) {
            LOG_ERROR("The Vulkan loader cannot load the software driver directly\n");
            return false;
        }
        if (!loadSoftwareDriver())
            return false;
        driverInfo.pfnGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddrLUNARG>(m_softwareGetInstanceProcAddr);
        // Exclusive, so a broken GPU driver cannot get in the way.
        driverList.mode = VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG;
        driverList.driverCount = 1;
        driverList.pDrivers = &driverInfo;
        builder.enable_extension(VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME);
        g_directDrivers = &driverList;
    }

    if (enableValidation)
        builder.request_validation_layers(true).use_default_debug_messenger();
    auto instRet = builder.build();
    g_directDrivers = nullptr;
    if (!instRet) {
        LOG_ERROR("Failed to create instance: " << instRet.error().message() << "\n");
        return false;
    }

    m_vkbInstance = instRet.value();
    volkLoadInstance(m_vkbInstance.instance);
    m_instance = vk::Instance(m_vkbInstance.instance);
    vk::detail::defaultDispatchLoaderDynamic.init(m_instance);
    return true;
}

bool VulkanContext::loadSoftwareDriver()
{
    if (!m_softwareDriver) {
        m_softwareDriver = SDL_LoadObject(toUtf8(runtimePath("vulkan_lvp.dll")).c_str());
        if (!m_softwareDriver) {
            LOG_ERROR("Failed to load the software Vulkan driver: " << SDL_GetError() << "\n");
            return false;
        }
    }
    m_softwareGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        SDL_LoadFunction(m_softwareDriver, "vk_icdGetInstanceProcAddr"));
    return m_softwareGetInstanceProcAddr != nullptr;
}

bool VulkanContext::createSurface(SDL_Window* window)
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, m_instance, nullptr, &surface)) {
        LOG_ERROR("Failed to create Vulkan surface\n");
        return false;
    }
    m_surface = vk::SurfaceKHR(surface);
    return true;
}

bool VulkanContext::createDevice()
{
    VkPhysicalDeviceFeatures coreFeatures{};
    // Per-draw data is looked up by gl_InstanceIndex, which the indirect draws set through firstInstance.
    coreFeatures.drawIndirectFirstInstance = VK_TRUE;

    VkPhysicalDeviceVulkan12Features features12{};
    features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    features12.separateDepthStencilLayouts = VK_TRUE;

    // Core in 1.3 and extensions before it; these structs work for both, unlike VkPhysicalDeviceVulkan13Features.
    VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{};
    dynamicRendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
    dynamicRendering.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceSynchronization2Features synchronization2{};
    synchronization2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
    synchronization2.synchronization2 = VK_TRUE;
    VkPhysicalDeviceShaderObjectFeaturesEXT shaderObject{};
    shaderObject.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT;
    shaderObject.shaderObject = VK_TRUE;

    vkb::PhysicalDeviceSelector selector{ m_vkbInstance };
    auto physRet = selector
        .set_minimum_version(1, 2)
        .add_required_extension(VK_EXT_SHADER_OBJECT_EXTENSION_NAME)
        .add_required_extension_features(dynamicRendering)
        .add_required_extension_features(synchronization2)
        .add_required_extension_features(shaderObject)
        .set_required_features(coreFeatures)
        .set_required_features_12(features12)
        .set_surface(m_surface)
        .select();
    if (!physRet) {
        LOG_ERROR("Failed to select physical device: " << physRet.error().message() << "\n");
        return false;
    }

    m_vkbPhysicalDevice = physRet.value();
    const uint32_t deviceVersion = m_vkbPhysicalDevice.properties.apiVersion;
    if (deviceVersion < VK_API_VERSION_1_3 &&
        (!m_vkbPhysicalDevice.enable_extension_if_present(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) ||
         !m_vkbPhysicalDevice.enable_extension_if_present(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME))) {
        LOG_ERROR("Dynamic rendering or synchronization2 is missing on a Vulkan 1.2 device\n");
        return false;
    }
    m_apiVersion = std::min(VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(deviceVersion), VK_API_VERSION_MINOR(deviceVersion), 0),
        kTargetApiVersion);

    const auto enableIfPresent = [this](VkBool32 VkPhysicalDeviceFeatures::* feature) {
        VkPhysicalDeviceFeatures wanted{};
        wanted.*feature = VK_TRUE;
        return m_vkbPhysicalDevice.enable_features_if_present(wanted);
    };
    m_features.wideLines = enableIfPresent(&VkPhysicalDeviceFeatures::wideLines);
    m_features.depthClamp = enableIfPresent(&VkPhysicalDeviceFeatures::depthClamp);
    m_features.samplerAnisotropy = enableIfPresent(&VkPhysicalDeviceFeatures::samplerAnisotropy);
    m_features.multiDrawIndirect = enableIfPresent(&VkPhysicalDeviceFeatures::multiDrawIndirect);
    m_physicalDevice = vk::PhysicalDevice(m_vkbPhysicalDevice.physical_device);

    auto deviceRet = vkb::DeviceBuilder{ m_vkbPhysicalDevice }.build();
    if (!deviceRet) {
        LOG_ERROR("Failed to create device: " << deviceRet.error().message() << "\n");
        return false;
    }

    m_vkbDevice = deviceRet.value();
    volkLoadDevice(m_vkbDevice.device);
    m_device = vk::Device(m_vkbDevice.device);
    vk::detail::defaultDispatchLoaderDynamic.init(m_device);

    auto graphicsQueue = m_vkbDevice.get_queue(vkb::QueueType::graphics);
    auto presentQueue = m_vkbDevice.get_queue(vkb::QueueType::present);
    auto graphicsFamily = m_vkbDevice.get_queue_index(vkb::QueueType::graphics);
    if (!graphicsQueue || !presentQueue || !graphicsFamily) {
        LOG_ERROR("Failed to get device queues\n");
        return false;
    }
    m_graphicsQueue = vk::Queue(graphicsQueue.value());
    m_presentQueue = vk::Queue(presentQueue.value());
    m_graphicsQueueFamily = graphicsFamily.value();
    return true;
}

bool VulkanContext::createAllocator()
{
    VmaVulkanFunctions vulkanFunctions{};
    vulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = m_physicalDevice;
    allocatorInfo.device = m_device;
    allocatorInfo.instance = m_instance;
    allocatorInfo.pVulkanFunctions = &vulkanFunctions;
    allocatorInfo.vulkanApiVersion = m_apiVersion;

    if (vmaCreateAllocator(&allocatorInfo, &m_allocator) != VK_SUCCESS) {
        LOG_ERROR("Failed to create VMA allocator\n");
        m_allocator = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

void VulkanContext::destroyDevice()
{
    if (m_device) {
        vkb::destroy_device(m_vkbDevice);
        m_vkbDevice = {};
        m_device = nullptr;
    }
    m_vkbPhysicalDevice = {};
    m_physicalDevice = nullptr;
    m_graphicsQueue = nullptr;
    m_presentQueue = nullptr;
    m_features = {};
}

void VulkanContext::destroyInstance()
{
    if (m_surface) {
        vkb::destroy_surface(m_vkbInstance, m_surface);
        m_surface = nullptr;
    }
    if (m_instance) {
        vkb::destroy_instance(m_vkbInstance);
        m_vkbInstance = {};
        m_instance = nullptr;
    }
}

void VulkanContext::shutdown()
{
    if (m_allocator) {
        vmaDestroyAllocator(m_allocator);
        m_allocator = VK_NULL_HANDLE;
    }
    destroyDevice();
    destroyInstance();
    if (m_softwareDriver) {
        SDL_UnloadObject(m_softwareDriver);
        m_softwareDriver = nullptr;
        m_softwareGetInstanceProcAddr = nullptr;
    }
    if (m_libraryLoaded) {
        SDL_Vulkan_UnloadLibrary();
        m_libraryLoaded = false;
    }
}
