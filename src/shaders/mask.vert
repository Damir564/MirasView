#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame_ubo.glsl"
#include "draw_data.glsl"

layout(location = 0) in vec3 inPosition;

// Same expression and qualifier as triangle.vert so depths compare exactly.
invariant gl_Position;

void main() {
    uint drawIndex = gl_InstanceIndex;
    TransformData t = transforms[draws[drawIndex].transformIndex];
    vec4 worldPosition = t.model * vec4(inPosition, 1.0);
    gl_Position = ubo.proj * ubo.view * worldPosition;
}
