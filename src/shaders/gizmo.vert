#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame_ubo.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

layout(push_constant) uniform GizmoPush {
    mat4 modelMatrix;
} pc;

layout(location = 0) out vec3 fragColor;

void main() {
    gl_Position = ubo.proj * ubo.view * pc.modelMatrix * vec4(inPosition, 1.0);
    fragColor = inColor;
}
