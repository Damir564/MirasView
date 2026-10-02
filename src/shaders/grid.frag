#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame_ubo.glsl"

layout(location = 0) in vec2 inNdc;
layout(location = 0) out vec4 outColor;

// Coverage of grid lines spaced `spacing` apart, about one pixel wide.
float gridLines(vec2 coord, float spacing) {
    vec2 c = coord / spacing;
    vec2 derivative = max(fwidth(c), vec2(1e-6));
    vec2 g = abs(fract(c - 0.5) - 0.5) / derivative;
    float line = 1.0 - min(min(g.x, g.y), 1.0);
    // Fade out once lines get denser than a few pixels so the grid does not turn into moire.
    float density = max(derivative.x, derivative.y);
    return line * (1.0 - smoothstep(0.15, 0.5, density));
}

float axisLine(float coord) {
    float w = max(fwidth(coord), 1e-6);
    return 1.0 - min(abs(coord) / (w * 1.5), 1.0);
}

void main() {
    vec4 farPoint = ubo.invViewProj * vec4(inNdc, 0.5, 1.0);
    vec3 origin = ubo.cameraPos.xyz;
    vec3 dir = normalize(farPoint.xyz / farPoint.w - origin);
    float t = abs(dir.y) > 1e-6 ? -origin.y / dir.y : -1.0;
    vec3 world = origin + dir * max(t, 0.0);

    vec4 clip = ubo.proj * ubo.view * vec4(world, 1.0);
    float depth = clip.z / clip.w;
    // Derivatives above need every pixel of the quad to take part, so discard only at the end.
    bool valid = t > 0.0 && depth >= 0.0 && depth <= 1.0;
    gl_FragDepth = valid ? depth : 1.0;

    float minor = gridLines(world.xz, 1.0);
    float major = gridLines(world.xz, 10.0);
    vec3 color = vec3(0.3);
    float alpha = max(minor * 0.35, major * 0.6);

    float xAxis = axisLine(world.z); // the X axis is the line z = 0
    float zAxis = axisLine(world.x);
    color = mix(color, vec3(0.9, 0.15, 0.15), xAxis);
    alpha = max(alpha, xAxis * 0.9);
    color = mix(color, vec3(0.15, 0.3, 0.95), zAxis);
    alpha = max(alpha, zAxis * 0.9);

    float fadeRadius = clamp(abs(origin.y) * 60.0, 60.0, max(ubo.farPlane * 0.5, 60.0));
    alpha *= 1.0 - smoothstep(fadeRadius * 0.3, fadeRadius, length(world.xz - origin.xz));
    if (!valid || alpha < 0.002)
        discard;
    outColor = vec4(color, alpha);
}
