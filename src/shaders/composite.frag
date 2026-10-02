#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame_ubo.glsl"

// HDR scene -> display: bloom, exposure, color grading, tone mapping, vignette, solid background, dither.
layout(set = 1, binding = 0) uniform sampler2D hdrColor;
layout(set = 2, binding = 0) uniform sampler2D bloomTexture;

layout(push_constant) uniform CompositePush {
    vec4 bloom; // x = strength (0 = off), y = 1 / mip count, zw = pixel -> bloom UV scale
    vec4 tone;  // x = exposure multiplier, y = tonemapper, z = contrast, w = saturation
    vec4 misc;  // x = vignette
} pc;

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

const int TONEMAP_ACES = 0;
const int TONEMAP_AGX = 1;
const int TONEMAP_NEUTRAL = 2;
const int TONEMAP_REINHARD = 3;

// Stephen Hill's fit of the ACES RRT + sRGB ODT.
vec3 tonemapAces(vec3 color) {
    const mat3 inputMatrix = mat3(0.59719, 0.07600, 0.02840, 0.35458, 0.90834, 0.13383, 0.04823, 0.01566, 0.83777);
    const mat3 outputMatrix = mat3(1.60475, -0.10208, -0.00327, -0.53108, 1.10813, -0.07276, -0.07367, -0.00605, 1.07602);
    // The fit maps middle grey rather dark; this keeps the overall brightness close to the other operators.
    color = inputMatrix * (color * 1.6);
    vec3 a = color * (color + 0.0245786) - 0.000090537;
    vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return clamp(outputMatrix * (a / b), 0.0, 1.0);
}

// AgX with the default contrast look (polynomial fit by Benjamin Wrensch).
vec3 tonemapAgx(vec3 color) {
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    vec3 x = clamp(log2(max(inset * color, vec3(1e-10))), minEv, maxEv);
    x = (x - minEv) / (maxEv - minEv);
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    x = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
    // The curve outputs display-encoded values; decode back to linear for the sRGB target.
    return pow(max(outset * x, vec3(0.0)), vec3(2.2));
}

// Khronos PBR Neutral: leaves colors below the highlights nearly untouched.
vec3 tonemapNeutral(vec3 color) {
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression)
        return color;
    const float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(color, vec3(newPeak), g);
}

vec3 tonemapReinhard(vec3 color) {
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    return color / (1.0 + luminance);
}

vec3 linearToSrgb(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 srgbToLinear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
    vec4 hdr = texelFetch(hdrColor, ivec2(gl_FragCoord.xy), 0);
    // In the solid-background mode the scene is premultiplied by its coverage (1 everywhere otherwise).
    float coverage = clamp(hdr.a, 0.0, 1.0);
    vec3 color = coverage > 1e-4 ? hdr.rgb / coverage : vec3(0.0);

    if (pc.bloom.x > 0.0) {
        vec3 bloom = textureLod(bloomTexture, gl_FragCoord.xy * pc.bloom.zw, 0.0).rgb * pc.bloom.y;
        color = mix(color, bloom, pc.bloom.x);
    }

    color *= pc.tone.x;
    float luminance = dot(color, vec3(0.2126, 0.7152, 0.0722));
    color = max(mix(vec3(luminance), color, pc.tone.w), vec3(0.0));
    // Contrast pivots around middle grey in log space, as film grading does.
    color = 0.18 * pow(color / 0.18, vec3(pc.tone.z));

    int tonemapper = int(pc.tone.y);
    if (tonemapper == TONEMAP_AGX)
        color = tonemapAgx(color);
    else if (tonemapper == TONEMAP_NEUTRAL)
        color = tonemapNeutral(color);
    else if (tonemapper == TONEMAP_REINHARD)
        color = tonemapReinhard(color);
    else
        color = tonemapAces(color);

    vec2 uv = (gl_FragCoord.xy - ubo.viewport.xy) / ubo.viewport.zw - 0.5;
    uv.x *= ubo.viewport.z / ubo.viewport.w;
    color *= 1.0 - pc.misc.x * smoothstep(0.25, 1.1, length(uv));

    color = mix(ubo.backgroundColor.rgb, color, coverage);

    // Triangular noise of +-1 LSB where the 8-bit quantization happens (sRGB) removes banding in gradients.
    float noise = interleavedGradientNoise(gl_FragCoord.xy) + interleavedGradientNoise(gl_FragCoord.yx + 13.7) - 1.0;
    color = srgbToLinear(clamp(linearToSrgb(clamp(color, 0.0, 1.0)) + noise / 255.0, 0.0, 1.0));
    outColor = vec4(color, 1.0);
}
