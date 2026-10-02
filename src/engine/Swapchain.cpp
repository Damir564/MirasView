#include "Swapchain.h"
#include "VulkanContext.h"
#include "Log.h"

Swapchain::~Swapchain()
{
    destroy();
}

bool Swapchain::create(VulkanContext& context, uint32_t width, uint32_t height, bool vsync)
{
    destroy();
    m_context = &context;
    std::string error;
    if (!build(width, height, vsync, VK_NULL_HANDLE, error)) {
        LOG_ERROR("Failed to create Swapchain: " << error << "\n");
        return false;
    }
    return true;
}

bool Swapchain::recreate(uint32_t width, uint32_t height, bool vsync)
{
    // SDL can still report the old size while the window is being minimized; the surface
    // extent is authoritative and a swapchain cannot be created while it is 0x0.
    auto surfaceCaps = m_context->physicalDevice().getSurfaceCapabilitiesKHR(m_context->surface());
    if (surfaceCaps.result != vk::Result::eSuccess ||
        surfaceCaps.value.currentExtent.width == 0 || surfaceCaps.value.currentExtent.height == 0)
        return false;

    (void)m_context->device().waitIdle();

    std::string error;
    if (!build(width, height, vsync, m_swapchain.swapchain, error)) {
        LOG_ERROR("Failed to recreate swapchain: " << error << "\n");
        return false;
    }
    return true;
}

bool Swapchain::build(uint32_t width, uint32_t height, bool vsync, VkSwapchainKHR oldSwapchain, std::string& error)
{
    vkb::SwapchainBuilder builder{ m_context->vkbDevice() };
    builder.set_old_swapchain(oldSwapchain)
        .set_desired_extent(width, height)
        .set_desired_format(VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR });
    if (vsync) {
        builder.set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR);
    }
    else {
        builder.set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
            .add_fallback_present_mode(VK_PRESENT_MODE_IMMEDIATE_KHR)
            .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR);
    }

    auto swapRet = builder.build();
    if (!swapRet) {
        error = swapRet.error().message();
        return false;
    }
    vkb::Swapchain newSwapchain = swapRet.value();

    auto imagesRet = newSwapchain.get_images();
    auto viewsRet = newSwapchain.get_image_views();
    if (!imagesRet || !viewsRet) {
        error = !imagesRet ? "failed to get swapchain images" : "failed to get swapchain image views";
        vkb::destroy_swapchain(newSwapchain);
        return false;
    }

    release();
    m_swapchain = newSwapchain;
    m_images = imagesRet.value();
    m_imageViews = viewsRet.value();
    return true;
}

void Swapchain::release()
{
    if (m_swapchain.swapchain == VK_NULL_HANDLE)
        return;
    m_swapchain.destroy_image_views(m_imageViews);
    vkb::destroy_swapchain(m_swapchain);
    m_swapchain = {};
    m_images.clear();
    m_imageViews.clear();
}

void Swapchain::destroy()
{
    release();
    m_context = nullptr;
}
