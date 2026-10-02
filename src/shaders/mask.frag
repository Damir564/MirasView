#version 460

// Resolved single-sample scene depth.
layout(set = 1, binding = 0) uniform sampler2D sceneDepth;

layout(location = 0) out vec2 outMask;

// R = selection silhouette (ignores depth), G = the part of it that is visible in the scene.
void main() {
    float scene = texelFetch(sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
    float tolerance = fwidth(gl_FragCoord.z) * 2.0 + 1e-6;
    float visible = gl_FragCoord.z <= scene + tolerance ? 1.0 : 0.0;
    outMask = vec2(1.0, visible);
}
