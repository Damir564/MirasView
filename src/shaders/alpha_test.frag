#version 460
#extension GL_GOOGLE_include_directive : require

#include "draw_data.glsl"

// Depth-only passes (shadow maps and the depth prepass): only alpha-tested materials do any work.
layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) flat in uint fragDrawIndex;

// Base color texture, for alpha testing.
layout(set = 1, binding = 0) uniform sampler2D baseColorSampler;

void main() {
    DrawData d = draws[fragDrawIndex];
    // Only MASK (1) needs the test; BLEND submeshes are never drawn into depth-only passes.
    if (d.alphaMode == 1 && texture(baseColorSampler, fragTexCoord).a * d.baseColor.a < d.alphaCutoff)
        discard;
}
