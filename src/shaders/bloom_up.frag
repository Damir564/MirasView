#version 460

// One step up the bloom chain: a 3x3 tent filter of the smaller mip, added onto the larger one.
layout(set = 1, binding = 0) uniform sampler2D source;

layout(push_constant) uniform BloomPush {
    vec4 texel;   // xy = 1 / source size
    vec4 uvClamp; // source UVs stay inside [xy, zw]
} pc;

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

vec3 fetch(vec2 uv, vec2 offset) {
    return textureLod(source, clamp(uv + offset * pc.texel.xy, pc.uvClamp.xy, pc.uvClamp.zw), 0.0).rgb;
}

void main() {
    vec2 uv = gl_FragCoord.xy * 0.5 * pc.texel.xy;
    vec3 color = fetch(uv, vec2(0.0)) * 4.0 +
        (fetch(uv, vec2(0.0, -1.0)) + fetch(uv, vec2(-1.0, 0.0)) +
         fetch(uv, vec2(1.0, 0.0)) + fetch(uv, vec2(0.0, 1.0))) * 2.0 +
        fetch(uv, vec2(-1.0, -1.0)) + fetch(uv, vec2(1.0, -1.0)) +
        fetch(uv, vec2(-1.0, 1.0)) + fetch(uv, vec2(1.0, 1.0));
    outColor = vec4(color / 16.0, 1.0);
}
