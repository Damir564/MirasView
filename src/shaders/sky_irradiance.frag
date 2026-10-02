#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame_ubo.glsl"
#include "sky.glsl"

// Cosine-weighted integral of the sky LUT for every normal direction: the diffuse light the sky and the
// sunlit ground give a surface. Laid out like the LUT.
layout(set = 1, binding = 0) uniform sampler2D skyLut;

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

void main() {
    vec3 n = skyDirection(inNdc * 0.5 + 0.5);
    const int COUNT = 256;
    const float GOLDEN_ANGLE = 2.39996323;
    vec3 irradiance = vec3(0.0);
    // Evenly spread directions (Fibonacci sphere), each covering the same solid angle.
    for (int i = 0; i < COUNT; ++i) {
        float y = 1.0 - (float(i) + 0.5) / float(COUNT) * 2.0;
        float ringRadius = sqrt(1.0 - y * y);
        float phi = GOLDEN_ANGLE * float(i);
        vec3 w = vec3(cos(phi) * ringRadius, y, sin(phi) * ringRadius);
        float cosine = dot(n, w);
        if (cosine > 0.0)
            irradiance += textureLod(skyLut, skyUv(w), 2.0).rgb * cosine;
    }
    outColor = vec4(irradiance * (4.0 * PI / float(COUNT)), 1.0);
}
