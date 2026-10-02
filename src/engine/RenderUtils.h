#pragma once
#include <vulkan/vulkan.hpp>
#include <cstdint>
#include <span>
#include <utility>
#include "Log.h"

// Helpers shared by the renderer's source files.

template <typename T>
bool takeResult(vk::ResultValue<T>&& created, T& out, const char* what)
{
    if (created.result != vk::Result::eSuccess) {
        LOG_ERROR("Failed to create " << what << ": " << vk::to_string(created.result) << "\n");
        return false;
    }
    out = std::move(created.value);
    return true;
}

inline vk::ImageMemoryBarrier2 imageBarrier(vk::Image image, vk::ImageAspectFlags aspect,
    vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
    vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess,
    vk::ImageLayout oldLayout, vk::ImageLayout newLayout,
    uint32_t baseMip = 0, uint32_t mipCount = 1, uint32_t baseLayer = 0, uint32_t layerCount = 1)
{
    vk::ImageMemoryBarrier2 barrier{};
    barrier.setSrcStageMask(srcStage)
        .setSrcAccessMask(srcAccess)
        .setDstStageMask(dstStage)
        .setDstAccessMask(dstAccess)
        .setOldLayout(oldLayout)
        .setNewLayout(newLayout)
        .setImage(image)
        .setSubresourceRange({ aspect, baseMip, mipCount, baseLayer, layerCount });
    return barrier;
}

inline void pipelineBarriers(vk::CommandBuffer cmd, std::span<const vk::ImageMemoryBarrier2> barriers)
{
    if (barriers.empty())
        return;
    vk::DependencyInfo info{};
    info.setImageMemoryBarrierCount(static_cast<uint32_t>(barriers.size()))
        .setPImageMemoryBarriers(barriers.data());
    cmd.pipelineBarrier2(info);
}

// Passes without a depth or stencil attachment still name one, with a null view: the shader object emulation
// layer only updates the attachment formats it builds pipelines for from the attachments a pass names.
inline void beginRendering(vk::CommandBuffer cmd, vk::RenderingInfo info)
{
    static const vk::RenderingAttachmentInfo kNoAttachment{};
    if (!info.pDepthAttachment)
        info.pDepthAttachment = &kNoAttachment;
    if (!info.pStencilAttachment)
        info.pStencilAttachment = &kNoAttachment;
    cmd.beginRendering(info);
}

inline vk::Viewport viewportFor(const vk::Rect2D& rect)
{
    return { float(rect.offset.x), float(rect.offset.y), float(rect.extent.width), float(rect.extent.height), 0.f, 1.f };
}
