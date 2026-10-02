// Shared FrameUBO block. Must match FrameUBO in engine/RenderTypes.h (std140).
#define MAX_CASCADES 4

const float PI = 3.14159265359;

layout(set = 0, binding = 0) uniform FrameUBO {
    mat4 view;
    mat4 proj;
    mat4 invViewProj;
    mat4 cascadeMatrices[MAX_CASCADES];
    vec4 cascadeSplits;     // view depth where each cascade ends
    vec4 cascadeParams[MAX_CASCADES]; // x = texel world size, y = depth range (m), z = 1 / diameter
    vec4 cameraPos;
    vec4 lightDir;          // xyz = direction the sunlight travels, w = sun angular radius
    vec4 sunColor;          // sun irradiance at the ground, zero while the sun is off
    vec4 sunTopColor;       // sun irradiance above the atmosphere
    vec4 backgroundColor;   // rgb = solid background (linear), w = 1 for the realistic sky
    vec4 atmosphereParams;  // x = haze, y = ground albedo
    vec4 fogParams;         // x = enabled, y = density at height 0, z = height falloff, w = far fade start
    vec4 shadowParams;      // x = enabled, y = cascade count, z = fade distance, w = soft shadows
    vec4 shadowParams2;     // x = 1 / map size, y = penumbra per meter of caster distance
    vec4 aoParams;          // x = enabled, y = radius, z = intensity, w = sample count
    vec4 aoParams2;         // x = contact shadows enabled
    vec4 projParams;        // x = 1 / proj[0][0], y = 1 / proj[1][1]
    vec4 viewport;          // scene rectangle in framebuffer pixels: xy = offset, zw = size
    vec4 renderSize;        // xy = framebuffer size, zw = 1 / size
    float time;
    float nearPlane;
    float farPlane;
    uint frameIndex;
} ubo;

bool realisticSky() {
    return ubo.backgroundColor.w > 0.5;
}

// Device depth (0..1) -> distance along the view axis.
float linearizeDepth(float depth) {
    float n = ubo.nearPlane;
    float f = ubo.farPlane;
    return n * f / (f - depth * (f - n));
}

// View-space position of the framebuffer point `pixel` at view depth `z`.
vec3 viewPositionFromPixel(vec2 pixel, float z) {
    vec2 ndc = (pixel - ubo.viewport.xy) / ubo.viewport.zw * 2.0 - 1.0;
    return vec3(ndc.x * z * ubo.projParams.x, ndc.y * z * ubo.projParams.y, -z);
}

// Per-pixel pseudo-random value in [0, 1); turns sampling patterns into fine noise instead of banding.
float interleavedGradientNoise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// Fraction of the view ray to `worldPos` that is fogged: exponential height fog plus a fade near the
// far plane so geometry never pops out of existence there.
float fogAmount(vec3 worldPos, float distance) {
    if (ubo.fogParams.x < 0.5)
        return 0.0;
    float falloff = ubo.fogParams.z;
    float cameraHeight = clamp(ubo.cameraPos.y, -500.0, 5000.0);
    float t = clamp(falloff * (worldPos.y - cameraHeight), -30.0, 30.0);
    // Density integrated along the ray: the camera's density times the average relative density.
    float heightTerm = abs(t) > 1e-4 ? (1.0 - exp(-t)) / t : 1.0;
    float opticalDepth = ubo.fogParams.y * exp(-falloff * cameraHeight) * distance * heightTerm;
    float fog = 1.0 - exp(-max(opticalDepth, 0.0));
    return max(fog, smoothstep(ubo.fogParams.w, ubo.farPlane, distance));
}
