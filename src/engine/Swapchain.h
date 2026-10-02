#pragma once
#include <vulkan/vulkan.hpp>
#include <VkBootstrap.h>
#include <cstdint>
#include <string>
#include <vector>

class VulkanContext;

class Swapchain {
public:
    Swapchain() = default;
    ~Swapchain();

    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;

    // vsync = false prefers mailbox, then immediate, then FIFO; vsync = true requests FIFO only.
    bool create(VulkanContext& context, uint32_t width, uint32_t height, bool vsync = false);
    // Returns false when the surface extent is 0 (minimized) or creation failed; the old swapchain is kept then.
    bool recreate(uint32_t width, uint32_t height, bool vsync = false);
    void destroy();

    vk::SwapchainKHR handle() const { return vk::SwapchainKHR(m_swapchain.swapchain); }
    const std::vector<VkImage>& images() const { return m_images; }
    const std::vector<VkImageView>& imageViews() const { return m_imageViews; }
    vk::Extent2D extent() const { return { m_swapchain.extent.width, m_swapchain.extent.height }; }
    vk::Format format() const { return static_cast<vk::Format>(m_swapchain.image_format); }
    uint32_t imageCount() const { return static_cast<uint32_t>(m_images.size()); }

private:
    bool build(uint32_t width, uint32_t height, bool vsync, VkSwapchainKHR oldSwapchain, std::string& error);
    void release();

    VulkanContext* m_context = nullptr;
    vkb::Swapchain m_swapchain;
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_imageViews;
};
