#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame_ubo.glsl"
#include "sky.glsl"

// Renders the sky radiance LUT; only runs when the sun or the atmosphere settings change.
layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 dir = skyDirection(inNdc * 0.5 + 0.5);
    vec3 toSun = -normalize(ubo.lightDir.xyz);
    vec3 radiance = skyRadiance(dir, toSun, ubo.sunTopColor.rgb, ubo.atmosphereParams.x, ubo.atmosphereParams.y);
    outColor = vec4(radiance, 1.0);
}
