#version 460

// One step down the bloom chain: 13-tap filter from "Next Generation Post Processing in Call of Duty:
// Advanced Warfare" (Jimenez 2014).
layout(set = 1, binding = 0) uniform sampler2D source;

layout(push_constant) uniform BloomPush {
    vec4 texel;   // xy = 1 / source size, z = 1 on the first step (firefly filter)
    vec4 uvClamp; // source UVs stay inside [xy, zw]
} pc;

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

vec3 fetch(vec2 uv, vec2 offset) {
    vec3 c = textureLod(source, clamp(uv + offset * pc.texel.xy, pc.uvClamp.xy, pc.uvClamp.zw), 0.0).rgb;
    // A single NaN or infinity would otherwise spread over the whole screen.
    return any(isnan(c)) ? vec3(0.0) : min(c, vec3(60000.0));
}

float luminance(vec3 c) {
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Weighted towards dark samples so single very bright pixels (the sun, specular glints) do not flicker.
vec3 karisAverage(vec3 a, vec3 b, vec3 c, vec3 d) {
    float wa = 1.0 / (1.0 + luminance(a));
    float wb = 1.0 / (1.0 + luminance(b));
    float wc = 1.0 / (1.0 + luminance(c));
    float wd = 1.0 / (1.0 + luminance(d));
    return (a * wa + b * wb + c * wc + d * wd) / (wa + wb + wc + wd);
}

void main() {
    // Destination pixel centers sit on the corner between four source texels.
    vec2 uv = gl_FragCoord.xy * 2.0 * pc.texel.xy;
    vec3 a = fetch(uv, vec2(-2.0, -2.0));
    vec3 b = fetch(uv, vec2(0.0, -2.0));
    vec3 c = fetch(uv, vec2(2.0, -2.0));
    vec3 d = fetch(uv, vec2(-2.0, 0.0));
    vec3 e = fetch(uv, vec2(0.0, 0.0));
    vec3 f = fetch(uv, vec2(2.0, 0.0));
    vec3 g = fetch(uv, vec2(-2.0, 2.0));
    vec3 h = fetch(uv, vec2(0.0, 2.0));
    vec3 i = fetch(uv, vec2(2.0, 2.0));
    vec3 j = fetch(uv, vec2(-1.0, -1.0));
    vec3 k = fetch(uv, vec2(1.0, -1.0));
    vec3 l = fetch(uv, vec2(-1.0, 1.0));
    vec3 m = fetch(uv, vec2(1.0, 1.0));

    vec3 color;
    if (pc.texel.z > 0.5) {
        color = karisAverage(j, k, l, m) * 0.5 +
            (karisAverage(a, b, d, e) + karisAverage(b, c, e, f) +
             karisAverage(d, e, g, h) + karisAverage(e, f, h, i)) * 0.125;
    } else {
        color = e * 0.125 + (a + c + g + i) * 0.03125 + (b + d + f + h) * 0.0625 + (j + k + l + m) * 0.125;
    }
    outColor = vec4(color, 1.0);
}
