#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame_ubo.glsl"
#include "draw_data.glsl"
#include "sky.glsl"

layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormal;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) in mat3 TBN;
layout(location = 6) flat in uint fragDrawIndex;

layout(set = 1, binding = 0) uniform sampler2D baseColorSampler;
layout(set = 2, binding = 0) uniform sampler2D normalMapSampler;
layout(set = 3, binding = 0) uniform sampler2D mrSampler;
// Scene lighting (see Renderer::createDescriptors).
layout(set = 4, binding = 0) uniform sampler2DArrayShadow shadowMap;
layout(set = 4, binding = 1) uniform sampler2DArray shadowDepth;
layout(set = 4, binding = 2) uniform sampler2D aoTexture;  // half resolution: R = AO, G = contact shadow
layout(set = 4, binding = 3) uniform sampler2D aoDepth;    // half resolution view depth
layout(set = 4, binding = 4) uniform sampler2D skyLut;
layout(set = 4, binding = 5) uniform sampler2D skyIrradiance;

// Premultiplied by coverage in the solid-background mode (see composite.frag).
layout(location = 0) out vec4 outColor;

const int ALPHA_MODE_OPAQUE = 0;
const int ALPHA_MODE_MASK = 1;
const int ALPHA_MODE_BLEND = 2;

// Must match kCascadeOverlap in engine/Shadow.cpp.
const float CASCADE_BLEND = 0.1;
// Occluders further than this from a receiver are not searched for (bounds the soft-shadow kernel).
const float MAX_PENUMBRA_CASTER_DISTANCE = 25.0;
const float MAX_SHADOW_FILTER_TEXELS = 24.0;

const vec2 POISSON[16] = vec2[](
    vec2(-0.94201624, -0.39906216), vec2(0.94558609, -0.76890725),
    vec2(-0.09418410, -0.92938870), vec2(0.34495938, 0.29387760),
    vec2(-0.91588581, 0.45771432), vec2(-0.81544232, -0.87912464),
    vec2(-0.38277543, 0.27676845), vec2(0.97484398, 0.75648379),
    vec2(0.44323325, -0.97511554), vec2(0.53742981, -0.47373420),
    vec2(-0.26496911, -0.41893023), vec2(0.79197514, 0.19090188),
    vec2(-0.24188840, 0.99706507), vec2(-0.81409955, 0.91437590),
    vec2(0.19984126, 0.78641367), vec2(0.14383161, -0.14100790));

// The cascade covering this view depth. Near the end of a cascade, pixels are handed to the next one with
// a dithered probability, which hides the resolution change at the seam.
int selectCascade(float viewDepth, float noise) {
    int count = int(ubo.shadowParams.y);
    for (int c = 0; c < count; ++c) {
        float end = ubo.cascadeSplits[c];
        if (viewDepth < end) {
            float start = c == 0 ? 0.0 : ubo.cascadeSplits[c - 1];
            float blendStart = end - (end - start) * CASCADE_BLEND;
            if (c + 1 < count && viewDepth > blendStart && noise < (viewDepth - blendStart) / (end - blendStart))
                return c + 1;
            return c;
        }
    }
    return -1;
}

float filterShadow(vec3 coord, float layer, float radiusUv, mat2 rotation) {
    float lit = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 offset = rotation * POISSON[i] * radiusUv;
        lit += texture(shadowMap, vec4(coord.xy + offset, layer, coord.z));
    }
    return lit / 16.0;
}

// Average depth of the occluders around coord (PCSS blocker search); negative if there are none.
float averageBlockerDepth(vec3 coord, float layer, float searchUv, mat2 rotation) {
    float sum = 0.0;
    float count = 0.0;
    for (int i = 0; i < 16; ++i) {
        vec2 offset = rotation * POISSON[i] * searchUv;
        float depth = textureLod(shadowDepth, vec3(coord.xy + offset, layer), 0.0).r;
        if (depth < coord.z) {
            sum += depth;
            count += 1.0;
        }
    }
    return count > 0.0 ? sum / count : -1.0;
}

float sunShadow(vec3 worldPos, vec3 Ng, vec3 L, float viewDepth) {
    if (ubo.shadowParams.x < 0.5)
        return 1.0;
    float fadeEnd = ubo.shadowParams.z;
    float fade = smoothstep(fadeEnd * 0.85, fadeEnd, viewDepth);
    if (fade >= 1.0)
        return 1.0;
    float noise = interleavedGradientNoise(gl_FragCoord.xy);
    int cascade = selectCascade(viewDepth, noise);
    if (cascade < 0)
        return 1.0;

    vec4 params = ubo.cascadeParams[cascade];
    float texelWorld = params.x;
    float depthRange = params.y;
    float uvPerMeter = params.z;
    float texelUv = ubo.shadowParams2.x;

    // Normal offset scaled to the texel size removes acne without the peter-panning a large depth bias causes.
    float NdotL = clamp(dot(Ng, L), 0.0, 1.0);
    float tanTheta = min(sqrt(1.0 - NdotL * NdotL) / max(NdotL, 1e-3), 4.0);
    vec3 offsetPos = worldPos + Ng * texelWorld * (0.75 + 1.5 * (1.0 - NdotL));
    vec3 coord = (ubo.cascadeMatrices[cascade] * vec4(offsetPos, 1.0)).xyz;
    coord.xy = coord.xy * 0.5 + 0.5;
    float layer = float(cascade);

    coord.z -= 0.5 * texelWorld / depthRange;

    float angle = noise * 2.0 * PI;
    mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
    float radiusUv = 1.5 * texelUv;
    if (ubo.shadowParams.w > 0.5) {
        // Soft shadows (PCSS): the penumbra grows with the distance between receiver and occluder.
        float penumbraPerMeter = ubo.shadowParams2.y;
        float searchUv = penumbraPerMeter * min(depthRange, MAX_PENUMBRA_CASTER_DISTANCE) * uvPerMeter * 0.5;
        searchUv = clamp(searchUv, 2.0 * texelUv, MAX_SHADOW_FILTER_TEXELS * texelUv);
        float blocker = averageBlockerDepth(coord, layer, searchUv, rotation);
        if (blocker < 0.0)
            return 1.0;
        float casterDistance = (coord.z - blocker) * depthRange;
        radiusUv = clamp(penumbraPerMeter * casterDistance * uvPerMeter * 0.5, 1.5 * texelUv,
            MAX_SHADOW_FILTER_TEXELS * texelUv);
    }
    // A wider filter reaches further across sloped receivers; bias depth along the slope accordingly.
    float filterWorld = radiusUv / uvPerMeter;
    coord.z -= filterWorld * tanTheta * 0.5 / depthRange;

    float lit = filterShadow(coord, layer, radiusUv, rotation);
    return mix(lit, 1.0, fade);
}

// Joint bilateral upsample of the half-resolution AO: the 2x2 nearest texels, weighted by how close their
// depth is to this pixel's, so occlusion does not bleed across silhouettes.
vec2 sampleAmbientOcclusion(float viewDepth) {
    if (ubo.aoParams.x < 0.5 && ubo.aoParams2.x < 0.5)
        return vec2(1.0);
    ivec2 rectMin = ivec2(floor(ubo.viewport.xy * 0.5));
    ivec2 rectMax = ivec2(ceil((ubo.viewport.xy + ubo.viewport.zw) * 0.5)) - 1;
    vec2 f = (gl_FragCoord.xy - 0.5) * 0.5;
    ivec2 base = ivec2(floor(f));
    vec2 t = f - vec2(base);
    vec2 sum = vec2(0.0);
    float weightSum = 0.0;
    for (int i = 0; i < 4; ++i) {
        ivec2 offset = ivec2(i & 1, i >> 1);
        ivec2 q = clamp(base + offset, rectMin, rectMax);
        float bilinear = (offset.x == 1 ? t.x : 1.0 - t.x) * (offset.y == 1 ? t.y : 1.0 - t.y);
        float depth = texelFetch(aoDepth, q, 0).r;
        float similarity = 1.0 / (1e-3 + abs(depth - viewDepth) / (viewDepth * 0.02));
        float weight = bilinear * similarity + 1e-5;
        sum += texelFetch(aoTexture, q, 0).rg * weight;
        weightSum += weight;
    }
    return sum / weightSum;
}

float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float denom = (NdotH * NdotH * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness) *
           GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 calcLight(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic, float roughness, vec3 F0) {
    vec3 H = normalize(V + L);
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 specular = (NDF * G * F) / (4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    return (kD * albedo / PI + specular) * radiance * max(dot(N, L), 0.0);
}

// Split-sum environment BRDF, analytic fit (Karis, "Physically Based Shading on Mobile").
vec2 environmentBrdf(float NdotV, float roughness) {
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4(1.0, 0.0425, 1.04, -0.04);
    vec4 r = roughness * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * NdotV)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

// Light bouncing between occluding surfaces brightens (and tints) AO on light materials (Jimenez 2016).
vec3 multiBounceAO(float ao, vec3 albedo) {
    vec3 a = 2.0404 * albedo - 0.3324;
    vec3 b = -4.7951 * albedo + 0.6417;
    vec3 c = 2.7552 * albedo + 0.6903;
    return max(vec3(ao), ((ao * a + b) * ao + c) * ao);
}

// Lagarde, "Moving Frostbite to PBR".
float specularOcclusion(float NdotV, float ao, float roughness) {
    return clamp(pow(NdotV + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao, 0.0, 1.0);
}

void main() {
    DrawData d = draws[fragDrawIndex];
    vec4 texColor = texture(baseColorSampler, fragTexCoord);
    float finalAlpha = texColor.a * d.baseColor.a;

    if (d.alphaMode == ALPHA_MODE_MASK) {
        if (finalAlpha < d.alphaCutoff)
            discard;
        finalAlpha = 1.0;
    } else if (d.alphaMode == ALPHA_MODE_OPAQUE) {
        finalAlpha = 1.0;
    }
    if (finalAlpha < 0.001)
        discard;

    vec3 albedo = d.baseColor.rgb * texColor.rgb;

    vec3 normalMapValue = texture(normalMapSampler, fragTexCoord).rgb * 2.0 - 1.0;
    vec3 N = TBN * normalMapValue;
    vec3 Ng = normalize(fragNormal);
    N = dot(N, N) > 1e-12 ? normalize(N) : Ng;

    vec4 mrSample = texture(mrSampler, fragTexCoord);
    float metallic = clamp(d.metallic * mrSample.b, 0.0, 1.0);
    float roughness = clamp(d.roughness * mrSample.g, 0.05, 1.0);

    vec3 toCamera = ubo.cameraPos.xyz - fragWorldPos;
    float viewDistance = length(toCamera);
    vec3 V = toCamera / max(viewDistance, 1e-6);
    // Geometry is drawn without culling; light back faces as if seen from the front.
    if (!gl_FrontFacing && dot(Ng, V) < 0.0) {
        N = -N;
        Ng = -Ng;
    }
    float viewDepth = -(ubo.view * vec4(fragWorldPos, 1.0)).z;
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // AO and contact shadows describe the opaque surface behind a transparent one, not the surface itself.
    vec2 occlusion = d.alphaMode == ALPHA_MODE_BLEND ? vec2(1.0) : sampleAmbientOcclusion(viewDepth);

    vec3 L = -normalize(ubo.lightDir.xyz);
    vec3 direct = vec3(0.0);
    if (dot(N, L) > 0.0 && dot(ubo.sunColor.rgb, vec3(1.0)) > 0.0) {
        float shadow = sunShadow(fragWorldPos, Ng, L, viewDepth) * occlusion.y;
        if (shadow > 0.0)
            direct = calcLight(N, V, L, ubo.sunColor.rgb, albedo, metallic, roughness, F0) * shadow;
    }

    // Image-based ambient light from the sky model.
    float NdotV = max(dot(N, V), 1e-4);
    float ao = occlusion.x;
    vec3 kS = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 kD = (1.0 - kS) * (1.0 - metallic);
    vec3 irradiance = textureLod(skyIrradiance, skyUv(N), 0.0).rgb;
    vec3 diffuseAmbient = kD * albedo / PI * irradiance * multiBounceAO(ao, albedo);

    vec3 R = reflect(-V, N);
    // Reflection directions below the surface would show the ground through the object.
    float horizon = clamp(1.0 + dot(R, Ng), 0.0, 1.0);
    vec3 prefiltered = textureLod(skyLut, skyUv(R), skyLutLod(roughness)).rgb;
    vec2 brdf = environmentBrdf(NdotV, roughness);
    vec3 specularAmbient = prefiltered * (F0 * brdf.x + brdf.y) * horizon * horizon *
        specularOcclusion(NdotV, ao, roughness);

    vec3 color = direct + diffuseAmbient + specularAmbient;

    float fog = fogAmount(fragWorldPos, viewDistance);
    if (realisticSky()) {
        // Distant geometry takes on the color of the sky near the horizon behind it.
        vec3 fogDir = normalize(vec3(-V.x, max(-V.y, 0.02), -V.z));
        vec3 fogColor = textureLod(skyLut, skyUv(fogDir), 3.0).rgb;
        outColor = vec4(mix(color, fogColor, fog), finalAlpha);
    } else {
        // Fog fades the surface into the solid background; composite.frag blends by coverage.
        float coverage = finalAlpha * (1.0 - fog);
        outColor = d.alphaMode == ALPHA_MODE_BLEND ? vec4(color, coverage) : vec4(color * coverage, coverage);
    }
}
