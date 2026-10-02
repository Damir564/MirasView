#version 460

// Separable depth-aware blur of the half-resolution AO; runs horizontally, then vertically.
layout(set = 1, binding = 0) uniform sampler2D aoInput;
layout(set = 2, binding = 0) uniform sampler2D aoDepth;

layout(push_constant) uniform AoPush {
    ivec4 rect;      // half-resolution scene texels: xy = min, zw = max
    ivec4 direction; // xy = step between taps
} pc;

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec2 outOcclusion;

const float WEIGHTS[5] = float[](0.2270270, 0.1945946, 0.1216216, 0.0540541, 0.0162162);

void main() {
    ivec2 texel = ivec2(gl_FragCoord.xy);
    float depth = texelFetch(aoDepth, texel, 0).r;
    // Taps on another surface (a depth jump) are ignored instead of smearing AO across the edge.
    float tolerance = depth * 0.03 + 0.01;
    vec2 sum = texelFetch(aoInput, texel, 0).rg * WEIGHTS[0];
    float weightSum = WEIGHTS[0];
    for (int i = 1; i <= 4; ++i) {
        for (int side = -1; side <= 1; side += 2) {
            ivec2 q = clamp(texel + pc.direction.xy * (i * side), pc.rect.xy, pc.rect.zw);
            float w = WEIGHTS[i] * max(0.0, 1.0 - abs(texelFetch(aoDepth, q, 0).r - depth) / tolerance);
            sum += texelFetch(aoInput, q, 0).rg * w;
            weightSum += w;
        }
    }
    outOcclusion = sum / weightSum;
}
