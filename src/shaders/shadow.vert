#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame_ubo.glsl"
#include "draw_data.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inTangent;

layout(push_constant) uniform ShadowPush {
    uint cascade;
} pc;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) flat out uint fragDrawIndex;

void main() {
    uint drawIndex = gl_InstanceIndex;
    mat4 model = transforms[draws[drawIndex].transformIndex].model;
    gl_Position = ubo.cascadeMatrices[pc.cascade] * (model * vec4(inPosition, 1.0));
    fragTexCoord = inTexCoord;
    fragDrawIndex = drawIndex;
}
