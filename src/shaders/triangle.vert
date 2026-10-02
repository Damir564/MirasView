#version 460
#extension GL_GOOGLE_include_directive : require

#include "frame_ubo.glsl"
#include "draw_data.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec4 inTangent;

layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormal;
layout(location = 2) out vec2 fragTexCoord;
layout(location = 3) out mat3 TBN;
layout(location = 6) flat out uint fragDrawIndex;

// prepass.vert and mask.vert must produce bit-identical positions for their depth comparisons.
invariant gl_Position;

void main() {
    // firstInstance of each indirect command is the index of its DrawData entry
    uint drawIndex = gl_InstanceIndex;
    TransformData t = transforms[draws[drawIndex].transformIndex];

    vec4 worldPosition = t.model * vec4(inPosition, 1.0);
    gl_Position = ubo.proj * ubo.view * worldPosition;

    mat3 normalMatrix = mat3(t.normal);
    vec3 worldNormal = normalMatrix * inNormal;
    worldNormal = dot(worldNormal, worldNormal) > 1e-12 ? normalize(worldNormal) : vec3(0.0, 1.0, 0.0);

    // Meshes without UVs come with zero tangents; normalizing those gives NaN (black pixels).
    vec3 worldTangent = normalMatrix * inTangent.xyz;
    worldTangent -= worldNormal * dot(worldNormal, worldTangent);
    if (dot(worldTangent, worldTangent) < 1e-12) {
        vec3 axis = abs(worldNormal.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        worldTangent = cross(axis, worldNormal);
    }
    worldTangent = normalize(worldTangent);
    float handedness = inTangent.w < 0.0 ? -1.0 : 1.0;
    vec3 worldBitangent = cross(worldNormal, worldTangent) * handedness;

    fragWorldPos = vec3(worldPosition);
    fragNormal = worldNormal;
    fragTexCoord = inTexCoord;
    TBN = mat3(worldTangent, worldBitangent, worldNormal);
    fragDrawIndex = drawIndex;
}
