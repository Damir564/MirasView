#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include "Shadow.h"

// CPU mirrors of GLSL blocks. Layouts must match the shaders exactly, including explicit padding.

// Mirrors FrameUBO (set 0, binding 0) in shaders/frame_ubo.glsl (std140).
struct FrameUBO {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 invViewProj;
    glm::mat4 cascadeMatrices[kMaxShadowCascades]; // world -> shadow clip space
    glm::vec4 cascadeSplits;   // view depth where each cascade ends
    glm::vec4 cascadeParams[kMaxShadowCascades]; // x = texel world size, y = depth range (m), z = 1 / diameter
    glm::vec4 cameraPos;       // xyz; w unused
    glm::vec4 lightDir;        // xyz = direction the sunlight travels; w = sun angular radius (radians)
    glm::vec4 sunColor;        // rgb = sun irradiance at the ground (zero while the sun is off); w unused
    glm::vec4 sunTopColor;     // rgb = sun irradiance above the atmosphere (lights the sky); w unused
    glm::vec4 backgroundColor; // rgb = linear solid background color; w = 1 for the realistic sky
    glm::vec4 atmosphereParams; // x = haze, y = ground albedo; zw unused
    glm::vec4 fogParams;       // x = enabled, y = density at height 0 (1/m), z = height falloff (1/m), w = far fade start
    glm::vec4 shadowParams;    // x = enabled, y = cascade count, z = fade distance, w = soft shadows
    glm::vec4 shadowParams2;   // x = 1 / map size, y = penumbra per meter of caster distance; zw unused
    glm::vec4 aoParams;        // x = enabled, y = radius (m), z = intensity, w = sample count
    glm::vec4 aoParams2;       // x = contact shadows enabled; yzw unused
    glm::vec4 projParams;      // x = 1 / proj[0][0], y = 1 / proj[1][1]; zw unused
    glm::vec4 viewport;        // scene rectangle in framebuffer pixels: xy = offset, zw = size
    glm::vec4 renderSize;      // xy = framebuffer size, zw = 1 / size
    float time;
    float nearPlane;
    float farPlane;
    uint32_t frameIndex;
};
static_assert(sizeof(FrameUBO) == 768);

// Every effects shader (fullscreen passes, selection mask) shares one push constant range of this size;
// each interprets it with its own block below.
inline constexpr uint32_t kFxPushConstantSize = 64;

// Mirrors the push_constant block in shaders/outline.frag.
struct OutlinePushConstants {
    glm::vec4 color;          // rgb, a = alpha of the visible outline
    float occludedAlpha;
    float widthPixels;
    float _pad0;
    float _pad1;
};
static_assert(sizeof(OutlinePushConstants) == 32);

// Mirrors the push_constant block in shaders/ao_depth.frag, ao.frag and ao_blur.frag.
struct AoPushConstants {
    glm::ivec4 rect;      // pixels the pass may read: xy = min, zw = max (inclusive)
    glm::ivec4 direction; // xy = blur step (ao_blur.frag only); zw unused
};
static_assert(sizeof(AoPushConstants) == 32);

// Mirrors the push_constant block in shaders/bloom_down.frag and bloom_up.frag.
struct BloomPushConstants {
    glm::vec4 texel;   // xy = 1 / source size, z = 1 for the first downsample (firefly filter); w unused
    glm::vec4 uvClamp; // source UVs are clamped to [xy, zw] so no texel outside the scene is read
};
static_assert(sizeof(BloomPushConstants) == 32);

// Mirrors the push_constant block in shaders/composite.frag.
struct CompositePushConstants {
    glm::vec4 bloom; // x = strength (0 = off), y = 1 / mip count, zw = full-res pixel -> bloom UV scale
    glm::vec4 tone;  // x = exposure multiplier, y = tonemapper, z = contrast, w = saturation
    glm::vec4 misc;  // x = vignette; yzw unused
};
static_assert(sizeof(CompositePushConstants) == 48);

// Mirrors the push_constant block in shaders/fxaa.frag.
struct FxaaPushConstants {
    glm::vec4 texel;   // xy = 1 / source size; zw unused
    glm::vec4 uvClamp; // source UVs are clamped to [xy, zw]
};
static_assert(sizeof(FxaaPushConstants) == 32);

// Mirrors the push_constant block in shaders/shadow.vert.
struct ShadowPushConstants {
    uint32_t cascade;
    uint32_t _pad[3];
};
static_assert(sizeof(ShadowPushConstants) == 16);

// Mirrors DrawData (set 0, binding 1) in shaders/draw_data.glsl (std430), indexed by gl_InstanceIndex.
struct GpuDrawData {
    glm::vec4 baseColor{ 1.0f };
    uint32_t transformIndex = 0;
    int32_t alphaMode = 0;
    float metallic = 0.0f;
    float roughness = 0.5f;
    float alphaCutoff = 0.5f;
    float _pad[3]{};
};
static_assert(sizeof(GpuDrawData) == 48);

// Mirrors TransformData (set 0, binding 2) in shaders/draw_data.glsl (std430). The normal
// matrix is precomputed so shaders don't invert per vertex.
struct GpuTransform {
    glm::mat4 model{ 1.0f };
    glm::mat4 normal{ 1.0f };
};
static_assert(sizeof(GpuTransform) == 128);
