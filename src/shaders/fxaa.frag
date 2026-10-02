#version 460

// FXAA 3.11 style edge search on the tone-mapped image (after Timothy Lottes' reference).
layout(set = 1, binding = 0) uniform sampler2D ldrColor;

layout(push_constant) uniform FxaaPush {
    vec4 texel;   // xy = 1 / source size
    vec4 uvClamp; // source UVs stay inside [xy, zw]
} pc;

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

const float EDGE_THRESHOLD_MIN = 0.0312;
const float EDGE_THRESHOLD_MAX = 0.125;
const float SUBPIXEL_QUALITY = 0.75;
const int ITERATIONS = 12;
const float QUALITY[12] = float[](1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);

vec3 fetch(vec2 uv) {
    return textureLod(ldrColor, clamp(uv, pc.uvClamp.xy, pc.uvClamp.zw), 0.0).rgb;
}

// The source is sampled as linear; edges are judged on perceptual (roughly gamma) luma.
float luma(vec3 c) {
    return sqrt(dot(c, vec3(0.299, 0.587, 0.114)));
}

void main() {
    vec2 texel = pc.texel.xy;
    vec2 uv = gl_FragCoord.xy * texel;
    vec3 colorCenter = fetch(uv);
    float lumaCenter = luma(colorCenter);
    float lumaDown = luma(fetch(uv + vec2(0.0, -1.0) * texel));
    float lumaUp = luma(fetch(uv + vec2(0.0, 1.0) * texel));
    float lumaLeft = luma(fetch(uv + vec2(-1.0, 0.0) * texel));
    float lumaRight = luma(fetch(uv + vec2(1.0, 0.0) * texel));

    float lumaMin = min(lumaCenter, min(min(lumaDown, lumaUp), min(lumaLeft, lumaRight)));
    float lumaMax = max(lumaCenter, max(max(lumaDown, lumaUp), max(lumaLeft, lumaRight)));
    float lumaRange = lumaMax - lumaMin;
    if (lumaRange < max(EDGE_THRESHOLD_MIN, lumaMax * EDGE_THRESHOLD_MAX)) {
        outColor = vec4(colorCenter, 1.0);
        return;
    }

    float lumaDownLeft = luma(fetch(uv + vec2(-1.0, -1.0) * texel));
    float lumaUpRight = luma(fetch(uv + vec2(1.0, 1.0) * texel));
    float lumaUpLeft = luma(fetch(uv + vec2(-1.0, 1.0) * texel));
    float lumaDownRight = luma(fetch(uv + vec2(1.0, -1.0) * texel));

    float lumaDownUp = lumaDown + lumaUp;
    float lumaLeftRight = lumaLeft + lumaRight;
    float lumaLeftCorners = lumaDownLeft + lumaUpLeft;
    float lumaDownCorners = lumaDownLeft + lumaDownRight;
    float lumaRightCorners = lumaDownRight + lumaUpRight;
    float lumaUpCorners = lumaUpRight + lumaUpLeft;

    float edgeHorizontal = abs(-2.0 * lumaLeft + lumaLeftCorners) + abs(-2.0 * lumaCenter + lumaDownUp) * 2.0 +
        abs(-2.0 * lumaRight + lumaRightCorners);
    float edgeVertical = abs(-2.0 * lumaUp + lumaUpCorners) + abs(-2.0 * lumaCenter + lumaLeftRight) * 2.0 +
        abs(-2.0 * lumaDown + lumaDownCorners);
    bool isHorizontal = edgeHorizontal >= edgeVertical;

    // Which side of the pixel the edge is on.
    float luma1 = isHorizontal ? lumaDown : lumaLeft;
    float luma2 = isHorizontal ? lumaUp : lumaRight;
    float gradient1 = luma1 - lumaCenter;
    float gradient2 = luma2 - lumaCenter;
    bool is1Steepest = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25 * max(abs(gradient1), abs(gradient2));

    float stepLength = isHorizontal ? texel.y : texel.x;
    float lumaLocalAverage;
    if (is1Steepest) {
        stepLength = -stepLength;
        lumaLocalAverage = 0.5 * (luma1 + lumaCenter);
    } else {
        lumaLocalAverage = 0.5 * (luma2 + lumaCenter);
    }

    // Walk along the edge in both directions until its ends.
    vec2 currentUv = uv;
    if (isHorizontal)
        currentUv.y += stepLength * 0.5;
    else
        currentUv.x += stepLength * 0.5;
    vec2 offset = isHorizontal ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);
    vec2 uv1 = currentUv - offset;
    vec2 uv2 = currentUv + offset;
    float lumaEnd1 = luma(fetch(uv1)) - lumaLocalAverage;
    float lumaEnd2 = luma(fetch(uv2)) - lumaLocalAverage;
    bool reached1 = abs(lumaEnd1) >= gradientScaled;
    bool reached2 = abs(lumaEnd2) >= gradientScaled;
    if (!reached1)
        uv1 -= offset;
    if (!reached2)
        uv2 += offset;
    for (int i = 2; i < ITERATIONS && !(reached1 && reached2); ++i) {
        if (!reached1)
            lumaEnd1 = luma(fetch(uv1)) - lumaLocalAverage;
        if (!reached2)
            lumaEnd2 = luma(fetch(uv2)) - lumaLocalAverage;
        reached1 = abs(lumaEnd1) >= gradientScaled;
        reached2 = abs(lumaEnd2) >= gradientScaled;
        if (!reached1)
            uv1 -= offset * QUALITY[i];
        if (!reached2)
            uv2 += offset * QUALITY[i];
    }

    float distance1 = isHorizontal ? (uv.x - uv1.x) : (uv.y - uv1.y);
    float distance2 = isHorizontal ? (uv2.x - uv.x) : (uv2.y - uv.y);
    bool isDirection1 = distance1 < distance2;
    float distanceFinal = min(distance1, distance2);
    float edgeLength = distance1 + distance2;
    float pixelOffset = -distanceFinal / edgeLength + 0.5;
    bool isLumaCenterSmaller = lumaCenter < lumaLocalAverage;
    bool correctVariation = ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0) != isLumaCenterSmaller;
    float finalOffset = correctVariation ? pixelOffset : 0.0;

    // Sub-pixel aliasing (thin lines, single bright pixels).
    float lumaAverage = (1.0 / 12.0) * (2.0 * (lumaDownUp + lumaLeftRight) + lumaLeftCorners + lumaRightCorners);
    float subPixel1 = clamp(abs(lumaAverage - lumaCenter) / lumaRange, 0.0, 1.0);
    float subPixel2 = (-2.0 * subPixel1 + 3.0) * subPixel1 * subPixel1;
    finalOffset = max(finalOffset, subPixel2 * subPixel2 * SUBPIXEL_QUALITY);

    vec2 finalUv = uv;
    if (isHorizontal)
        finalUv.y += finalOffset * stepLength;
    else
        finalUv.x += finalOffset * stepLength;
    outColor = vec4(fetch(finalUv), 1.0);
}
