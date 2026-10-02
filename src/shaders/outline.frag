#version 460

// Selection mask: R = silhouette, G = visible part of it.
layout(set = 1, binding = 0) uniform sampler2D selectionMask;

layout(push_constant) uniform OutlinePush {
    vec4 color;          // rgb, a = alpha of the visible outline
    float occludedAlpha;
    float widthPixels;
    float _pad0;
    float _pad1;
} pc;

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

const int MAX_RADIUS = 4;

void main() {
    ivec2 size = textureSize(selectionMask, 0);
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (texelFetch(selectionMask, p, 0).r > 0.5)
        discard;

    float radius = clamp(pc.widthPixels, 1.0, float(MAX_RADIUS));
    float silhouette = 0.0;
    float visible = 0.0;
    for (int y = -MAX_RADIUS; y <= MAX_RADIUS; ++y) {
        for (int x = -MAX_RADIUS; x <= MAX_RADIUS; ++x) {
            float d = length(vec2(x, y));
            if (d > radius + 0.5)
                continue;
            vec2 m = texelFetch(selectionMask, clamp(p + ivec2(x, y), ivec2(0), size - 1), 0).rg;
            // Soft outer edge for a little anti-aliasing.
            float weight = clamp(radius + 0.5 - d, 0.0, 1.0);
            silhouette = max(silhouette, m.r * weight);
            visible = max(visible, m.g * weight);
        }
    }
    if (silhouette < 0.01)
        discard;
    float alpha = mix(pc.occludedAlpha, pc.color.a, visible) * silhouette;
    outColor = vec4(pc.color.rgb, alpha);
}
