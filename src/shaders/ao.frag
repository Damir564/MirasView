#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame_ubo.glsl"

// Half resolution: scalable ambient obscurance (McGuire et al. 2012) and screen-space contact shadows.
layout(set = 1, binding = 0) uniform sampler2D aoDepth;

layout(push_constant) uniform AoPush {
    ivec4 rect;      // half-resolution scene texels: xy = min, zw = max
    ivec4 direction;
} pc;

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec2 outOcclusion; // R = ambient occlusion, G = contact shadow

const float SPIRAL_TURNS = 7.0;
const float MAX_RADIUS_TEXELS = 96.0;
const int CONTACT_STEPS = 12;

// Texel t of the half-resolution depth stands for the full-resolution pixel 2t.
vec3 positionAt(ivec2 texel) {
    return viewPositionFromPixel(vec2(texel) * 2.0 + 0.5, texelFetch(aoDepth, texel, 0).r);
}

// Differences towards the neighbor with the smaller depth step, so silhouettes do not bend the normal.
vec3 reconstructNormal(ivec2 texel, vec3 P) {
    vec3 right = positionAt(min(texel + ivec2(1, 0), pc.rect.zw));
    vec3 left = positionAt(max(texel - ivec2(1, 0), pc.rect.xy));
    vec3 down = positionAt(min(texel + ivec2(0, 1), pc.rect.zw));
    vec3 up = positionAt(max(texel - ivec2(0, 1), pc.rect.xy));
    bool useRight = texel.x < pc.rect.z && (texel.x == pc.rect.x || abs(right.z - P.z) < abs(P.z - left.z));
    bool useDown = texel.y < pc.rect.w && (texel.y == pc.rect.y || abs(down.z - P.z) < abs(P.z - up.z));
    vec3 dx = useRight ? right - P : P - left;
    vec3 dy = useDown ? down - P : P - up;
    vec3 n = cross(dy, dx);
    if (dot(n, n) < 1e-20)
        return vec3(0.0, 0.0, 1.0);
    n = normalize(n);
    return dot(n, P) > 0.0 ? -n : n;
}

float ambientOcclusion(ivec2 texel, vec3 P, vec3 N, float noise) {
    float radius = ubo.aoParams.y;
    int samples = int(ubo.aoParams.w);
    // Half-resolution texels per meter at this depth.
    float texelsPerMeter = 0.25 * ubo.viewport.w / (abs(ubo.projParams.y) * -P.z);
    float diskRadius = min(radius * texelsPerMeter, MAX_RADIUS_TEXELS);
    if (diskRadius < 1.0)
        return 1.0;

    float radius2 = radius * radius;
    float bias = 0.02 * radius;
    float spin = noise * 2.0 * PI;
    float sum = 0.0;
    for (int i = 0; i < samples; ++i) {
        float a = (float(i) + 0.5) / float(samples);
        float angle = a * SPIRAL_TURNS * 2.0 * PI + spin;
        vec2 offset = vec2(cos(angle), sin(angle)) * (a * diskRadius);
        ivec2 q = clamp(texel + ivec2(round(offset)), pc.rect.xy, pc.rect.zw);
        vec3 v = positionAt(q) - P;
        float vv = dot(v, v);
        float vn = dot(v, N);
        float f = max(radius2 - vv, 0.0);
        sum += f * f * f * max((vn - bias) / (0.01 + vv), 0.0);
    }
    float intensity = ubo.aoParams.z;
    return max(0.0, 1.0 - sum * intensity * 5.0 / (radius2 * radius2 * radius2 * float(samples)));
}

// Marches a short ray towards the sun through the depth buffer. Catches the small-scale shadows (objects
// standing on a floor, window frames) that a shadow-map texel is too coarse for.
float contactShadow(vec3 P, vec3 N, float noise) {
    vec3 L = normalize(mat3(ubo.view) * -ubo.lightDir.xyz);
    if (dot(N, L) <= 0.0)
        return 1.0;
    float z = -P.z;
    float rayLength = clamp(0.02 * z, 0.15, 0.8);
    float thickness = max(0.5 * rayLength, 0.05);
    vec3 origin = P + N * (0.004 * z);
    for (int i = 0; i < CONTACT_STEPS; ++i) {
        float t = (float(i) + noise) / float(CONTACT_STEPS) * rayLength;
        vec3 S = origin + L * t;
        if (-S.z <= ubo.nearPlane)
            break;
        vec2 ndc = vec2(S.x / (-S.z * ubo.projParams.x), S.y / (-S.z * ubo.projParams.y));
        vec2 pixel = (ndc * 0.5 + 0.5) * ubo.viewport.zw + ubo.viewport.xy;
        ivec2 q = ivec2(floor(pixel * 0.5));
        if (any(lessThan(q, pc.rect.xy)) || any(greaterThan(q, pc.rect.zw)))
            break;
        float behind = -S.z - texelFetch(aoDepth, q, 0).r;
        if (behind > 0.01 * z && behind < thickness)
            return smoothstep(0.3, 1.0, t / rayLength);
    }
    return 1.0;
}

void main() {
    ivec2 texel = ivec2(gl_FragCoord.xy);
    float z = texelFetch(aoDepth, texel, 0).r;
    if (z >= ubo.farPlane * 0.999) {
        outOcclusion = vec2(1.0);
        return;
    }
    vec3 P = viewPositionFromPixel(vec2(texel) * 2.0 + 0.5, z);
    vec3 N = reconstructNormal(texel, P);
    float noise = interleavedGradientNoise(gl_FragCoord.xy);
    float ao = ubo.aoParams.x > 0.5 ? ambientOcclusion(texel, P, N, noise) : 1.0;
    float contact = ubo.aoParams2.x > 0.5 ? contactShadow(P, N, noise) : 1.0;
    outOcclusion = vec2(ao, contact);
}
