#include "Renderer.h"
#include "RenderUtils.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

// Fullscreen passes of the renderer: sky LUT, ambient occlusion, bloom and the composite settings.

namespace {

// Bloom setting (0..1) -> blend factor of the bloom chain.
constexpr float kBloomStrengthScale = 0.15f;

vk::Extent2D mipExtent(vk::Extent2D base, uint32_t level)
{
    return { std::max(1u, base.width >> level), std::max(1u, base.height >> level) };
}

// `rect` scaled down by 2^level, rounded outwards and clamped to `limit`.
vk::Rect2D scaledRect(const vk::Rect2D& rect, uint32_t level, vk::Extent2D limit)
{
    const int32_t round = (1 << level) - 1;
    const int32_t x0 = std::min(rect.offset.x >> level, static_cast<int32_t>(limit.width) - 1);
    const int32_t y0 = std::min(rect.offset.y >> level, static_cast<int32_t>(limit.height) - 1);
    const int32_t x1 = std::clamp((rect.offset.x + static_cast<int32_t>(rect.extent.width) + round) >> level,
        x0 + 1, static_cast<int32_t>(limit.width));
    const int32_t y1 = std::clamp((rect.offset.y + static_cast<int32_t>(rect.extent.height) + round) >> level,
        y0 + 1, static_cast<int32_t>(limit.height));
    return { { x0, y0 }, { static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0) } };
}

// Inclusive pixel bounds, as the AO shaders clamp their reads to.
glm::ivec4 rectBounds(const vk::Rect2D& rect)
{
    return { rect.offset.x, rect.offset.y, rect.offset.x + static_cast<int32_t>(rect.extent.width) - 1,
        rect.offset.y + static_cast<int32_t>(rect.extent.height) - 1 };
}

// UV range spanning the texel centers of `rect` in an image of `size`.
glm::vec4 uvClampFor(const vk::Rect2D& rect, vk::Extent2D size)
{
    return { (rect.offset.x + 0.5f) / size.width, (rect.offset.y + 0.5f) / size.height,
        (rect.offset.x + rect.extent.width - 0.5f) / size.width,
        (rect.offset.y + rect.extent.height - 0.5f) / size.height };
}

} // namespace

void Renderer::beginFxPass(vk::CommandBuffer cmd, vk::ImageView target, const vk::Rect2D& area,
    vk::AttachmentLoadOp loadOp) const
{
    vk::RenderingAttachmentInfo attachment{};
    attachment.setImageView(target)
        .setImageLayout(vk::ImageLayout::eColorAttachmentOptimal)
        .setLoadOp(loadOp)
        .setStoreOp(vk::AttachmentStoreOp::eStore);
    vk::RenderingInfo renderInfo{};
    renderInfo.setRenderArea(area).setLayerCount(1).setColorAttachments(attachment);
    beginRendering(cmd, renderInfo);
    setDefaultDrawState(cmd, vk::SampleCountFlagBits::e1, viewportFor(area), area);
}

void Renderer::drawFx(vk::CommandBuffer cmd, const ShaderPair& shaders, std::initializer_list<vk::DescriptorSet> inputs,
    const void* pushData, uint32_t pushSize) const
{
    bindShaderPair(cmd, shaders);
    cmd.setVertexInputEXT(0, nullptr, 0, nullptr);
    cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_fxLayout, 0, 1, &m_frameSets[m_currentFrame], 0, nullptr);
    if (inputs.size() > 0)
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, m_fxLayout, 1, static_cast<uint32_t>(inputs.size()),
            inputs.begin(), 0, nullptr);
    // The whole shared range is always written, so no shader reads undefined push constants.
    std::array<std::byte, kFxPushConstantSize> push{};
    if (pushSize > 0)
        std::memcpy(push.data(), pushData, std::min<size_t>(pushSize, push.size()));
    cmd.pushConstants(m_fxLayout, vk::ShaderStageFlagBits::eFragment, 0, kFxPushConstantSize, push.data());
    cmd.draw(3, 1, 0, 0);
}

void Renderer::colorTargetToSampled(vk::CommandBuffer cmd, const RenderImage& image, uint32_t mip) const
{
    const vk::ImageMemoryBarrier2 barrier = imageBarrier(vk::Image(image.image), vk::ImageAspectFlagBits::eColor,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderSampledRead,
        vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eShaderReadOnlyOptimal, mip, 1);
    pipelineBarriers(cmd, { &barrier, 1 });
}

// Sky radiance LUT, its mip chain and the irradiance derived from it. Only recorded when the sun or the
// atmosphere changed; every other frame reuses them.
void Renderer::recordSkyPasses(vk::CommandBuffer cmd)
{
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    using Layout = vk::ImageLayout;
    const vk::Image lut(m_skyLut.image);
    const auto color = vk::ImageAspectFlagBits::eColor;

    // Earlier frames only sampled these; everything is rebuilt.
    const vk::ImageMemoryBarrier2 toTargets[3] = {
        imageBarrier(lut, color, Stage::eFragmentShader, Access::eNone,
            Stage::eColorAttachmentOutput, Access::eColorAttachmentWrite,
            Layout::eUndefined, Layout::eColorAttachmentOptimal, 0, 1),
        imageBarrier(lut, color, Stage::eFragmentShader, Access::eNone, Stage::eBlit, Access::eTransferWrite,
            Layout::eUndefined, Layout::eTransferDstOptimal, 1, kSkyLutMips - 1),
        imageBarrier(vk::Image(m_skyIrradiance.image), color, Stage::eFragmentShader, Access::eNone,
            Stage::eColorAttachmentOutput, Access::eColorAttachmentWrite,
            Layout::eUndefined, Layout::eColorAttachmentOptimal),
    };
    pipelineBarriers(cmd, toTargets);

    beginFxPass(cmd, m_skyLutTargetView, { { 0, 0 }, kSkyLutExtent }, vk::AttachmentLoadOp::eDontCare);
    drawFx(cmd, m_skyLutShaders, {});
    cmd.endRendering();

    const vk::ImageMemoryBarrier2 mip0ToSource = imageBarrier(lut, color,
        Stage::eColorAttachmentOutput, Access::eColorAttachmentWrite, Stage::eBlit, Access::eTransferRead,
        Layout::eColorAttachmentOptimal, Layout::eTransferSrcOptimal, 0, 1);
    pipelineBarriers(cmd, { &mip0ToSource, 1 });

    for (uint32_t mip = 1; mip < kSkyLutMips; ++mip) {
        const vk::Extent2D src = mipExtent(kSkyLutExtent, mip - 1);
        const vk::Extent2D dst = mipExtent(kSkyLutExtent, mip);
        // vkCmdBlitImage rather than vkCmdBlitImage2, which would need VK_KHR_copy_commands2 on 1.2 devices.
        vk::ImageBlit blit{};
        blit.srcSubresource = vk::ImageSubresourceLayers{ color, mip - 1, 0, 1 };
        blit.srcOffsets[1] = vk::Offset3D(static_cast<int32_t>(src.width), static_cast<int32_t>(src.height), 1);
        blit.dstSubresource = vk::ImageSubresourceLayers{ color, mip, 0, 1 };
        blit.dstOffsets[1] = vk::Offset3D(static_cast<int32_t>(dst.width), static_cast<int32_t>(dst.height), 1);
        cmd.blitImage(lut, Layout::eTransferSrcOptimal, lut, Layout::eTransferDstOptimal, blit, vk::Filter::eLinear);

        const vk::ImageMemoryBarrier2 toSource = imageBarrier(lut, color,
            Stage::eBlit, Access::eTransferWrite, Stage::eBlit, Access::eTransferRead,
            Layout::eTransferDstOptimal, Layout::eTransferSrcOptimal, mip, 1);
        pipelineBarriers(cmd, { &toSource, 1 });
    }

    const vk::ImageMemoryBarrier2 lutToSampled = imageBarrier(lut, color,
        Stage::eBlit, Access::eTransferRead,
        Stage::eFragmentShader, Access::eShaderSampledRead,
        Layout::eTransferSrcOptimal, Layout::eShaderReadOnlyOptimal, 0, kSkyLutMips);
    pipelineBarriers(cmd, { &lutToSampled, 1 });

    beginFxPass(cmd, m_skyIrradiance.view, { { 0, 0 }, kSkyIrradianceExtent }, vk::AttachmentLoadOp::eDontCare);
    drawFx(cmd, m_skyIrradianceShaders, { m_skyLutSet });
    cmd.endRendering();
    colorTargetToSampled(cmd, m_skyIrradiance);
}

// Half-resolution depth, then AO + contact shadows, then a separable depth-aware blur that leaves the
// result in m_aoRaw.
void Renderer::recordAoPasses(vk::CommandBuffer cmd, const FrameInput& input)
{
    const vk::Rect2D fullRect = sceneRect(input);
    const vk::Rect2D halfRect = scaledRect(fullRect, 1, m_halfExtent);
    const auto loadOp = vk::AttachmentLoadOp::eDontCare;

    AoPushConstants push{};
    push.rect = rectBounds(fullRect);
    beginFxPass(cmd, m_aoDepth.view, halfRect, loadOp);
    drawFx(cmd, m_aoDepthShaders, { m_sceneDepthSet }, &push, sizeof(push));
    cmd.endRendering();
    colorTargetToSampled(cmd, m_aoDepth);

    push.rect = rectBounds(halfRect);
    beginFxPass(cmd, m_aoRaw.view, halfRect, loadOp);
    drawFx(cmd, m_aoShaders, { m_aoDepthSet }, &push, sizeof(push));
    cmd.endRendering();
    colorTargetToSampled(cmd, m_aoRaw);

    push.direction = glm::ivec4(1, 0, 0, 0);
    beginFxPass(cmd, m_aoTemp.view, halfRect, loadOp);
    drawFx(cmd, m_aoBlurShaders, { m_aoRawSet, m_aoDepthSet }, &push, sizeof(push));
    cmd.endRendering();
    colorTargetToSampled(cmd, m_aoTemp);

    const vk::ImageMemoryBarrier2 rawToTarget = imageBarrier(vk::Image(m_aoRaw.image), vk::ImageAspectFlagBits::eColor,
        vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eNone,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eColorAttachmentOptimal);
    pipelineBarriers(cmd, { &rawToTarget, 1 });

    push.direction = glm::ivec4(0, 1, 0, 0);
    beginFxPass(cmd, m_aoRaw.view, halfRect, loadOp);
    drawFx(cmd, m_aoBlurShaders, { m_aoTempSet, m_aoDepthSet }, &push, sizeof(push));
    cmd.endRendering();
    colorTargetToSampled(cmd, m_aoRaw);
}

// Downsamples the HDR scene through the mip chain, then adds each mip back onto the next larger one, so
// mip 0 ends up with the sum of increasingly wide blurs.
void Renderer::recordBloom(vk::CommandBuffer cmd, const FrameInput& input)
{
    const vk::Rect2D rect = sceneRect(input);
    const vk::Extent2D fullExtent = m_swapchain.extent();
    const auto loadOp = vk::AttachmentLoadOp::eDontCare;

    BloomPushConstants push{};
    for (uint32_t mip = 0; mip < m_bloomMips; ++mip) {
        const vk::Extent2D size = mipExtent(m_halfExtent, mip);
        const vk::Extent2D sourceSize = mip == 0 ? fullExtent : mipExtent(m_halfExtent, mip - 1);
        const vk::Rect2D sourceRect = mip == 0 ? rect : scaledRect(rect, mip, sourceSize);
        push.texel = glm::vec4(1.0f / sourceSize.width, 1.0f / sourceSize.height, mip == 0 ? 1.0f : 0.0f, 0.0f);
        push.uvClamp = uvClampFor(sourceRect, sourceSize);
        beginFxPass(cmd, m_bloomMipViews[mip], scaledRect(rect, mip + 1, size), loadOp);
        drawFx(cmd, m_bloomDownShaders, { mip == 0 ? m_hdrSet : m_bloomSets[mip - 1] }, &push, sizeof(push));
        cmd.endRendering();
        colorTargetToSampled(cmd, m_bloom, mip);
    }

    for (int mip = static_cast<int>(m_bloomMips) - 2; mip >= 0; --mip) {
        const uint32_t level = static_cast<uint32_t>(mip);
        const vk::Extent2D size = mipExtent(m_halfExtent, level);
        const vk::Extent2D sourceSize = mipExtent(m_halfExtent, level + 1);
        push.texel = glm::vec4(1.0f / sourceSize.width, 1.0f / sourceSize.height, 0.0f, 0.0f);
        push.uvClamp = uvClampFor(scaledRect(rect, level + 2, sourceSize), sourceSize);

        const vk::ImageMemoryBarrier2 toTarget = imageBarrier(vk::Image(m_bloom.image), vk::ImageAspectFlagBits::eColor,
            vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eNone,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
            vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageLayout::eColorAttachmentOptimal, level, 1);
        pipelineBarriers(cmd, { &toTarget, 1 });

        beginFxPass(cmd, m_bloomMipViews[level], scaledRect(rect, level + 1, size), vk::AttachmentLoadOp::eLoad);
        setAdditiveBlending(cmd);
        drawFx(cmd, m_bloomUpShaders, { m_bloomSets[level + 1] }, &push, sizeof(push));
        cmd.endRendering();
        colorTargetToSampled(cmd, m_bloom, level);
    }
}

CompositePushConstants Renderer::compositeConstants() const
{
    CompositePushConstants constants{};
    const float bloomStrength = m_settings.bloom ? m_settings.bloomIntensity * kBloomStrengthScale : 0.0f;
    constants.bloom = glm::vec4(bloomStrength, 1.0f / static_cast<float>(std::max(m_bloomMips, 1u)),
        0.5f / static_cast<float>(m_halfExtent.width), 0.5f / static_cast<float>(m_halfExtent.height));
    constants.tone = glm::vec4(std::exp2(m_settings.exposure), static_cast<float>(m_settings.tonemapper),
        m_settings.contrast, m_settings.saturation);
    constants.misc = glm::vec4(m_settings.vignette, 0.0f, 0.0f, 0.0f);
    return constants;
}
