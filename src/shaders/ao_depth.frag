#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame_ubo.glsl"

// Half-resolution view depth for the AO passes and for the bilateral upsample in triangle.frag.
layout(set = 1, binding = 0) uniform sampler2D sceneDepth;

layout(push_constant) uniform AoPush {
    ivec4 rect;      // full-resolution scene pixels: xy = min, zw = max
    ivec4 direction;
} pc;

layout(location = 0) in vec2 inNdc;
layout(location = 0) out float outDepth;

void main() {
    ivec2 p = clamp(ivec2(gl_FragCoord.xy) * 2, pc.rect.xy, pc.rect.zw);
    outDepth = linearizeDepth(texelFetch(sceneDepth, p, 0).r);
}
