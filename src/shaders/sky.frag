#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame_ubo.glsl"
#include "sky.glsl"

layout(set = 1, binding = 0) uniform sampler2D skyLut;

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

// The real disk is ~15000x brighter than its irradiance; this keeps it far above the sky (so it blooms)
// while staying well inside half-float range.
const float SUN_DISK_BRIGHTNESS = 1500.0;

void main() {
    vec4 world = ubo.invViewProj * vec4(inNdc, 0.5, 1.0);
    vec3 dir = normalize(world.xyz / world.w - ubo.cameraPos.xyz);
    vec3 color = textureLod(skyLut, skyUv(dir), 0.0).rgb;

    vec3 toSun = -normalize(ubo.lightDir.xyz);
    float angle = acos(clamp(dot(dir, toSun), -1.0, 1.0));
    float radius = ubo.lightDir.w;
    if (angle < radius * 2.0) {
        // About one pixel of anti-aliasing at the rim, then limb darkening towards it.
        float pixelAngle = 2.0 * abs(ubo.projParams.y) / ubo.viewport.w;
        float disk = 1.0 - smoothstep(radius - pixelAngle, radius + pixelAngle, angle);
        float mu = sqrt(max(1.0 - (angle * angle) / (radius * radius), 0.0));
        float limb = 1.0 - 0.6 * (1.0 - mu);
        color += ubo.sunColor.rgb * SUN_DISK_BRIGHTNESS * disk * limb;
    }
    outColor = vec4(color, 1.0);
}
