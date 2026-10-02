#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

struct Camera {
    glm::vec3 position{ 0.0f, 0.0f, 0.0f };
    float yaw = -90.0f;
    float pitch = 0.0f;
    float speed = 10.0f;
    float sensitivity = 0.1f;
};

inline glm::vec3 getFront(const Camera& cam) {
    return glm::normalize(glm::vec3(
        cos(glm::radians(cam.yaw)) * cos(glm::radians(cam.pitch)),
        sin(glm::radians(cam.pitch)),
        sin(glm::radians(cam.yaw)) * cos(glm::radians(cam.pitch))));
}

inline glm::mat4 getView(const Camera& cam) {
    return glm::lookAt(cam.position, cam.position + getFront(cam), glm::vec3(0, 1, 0));
}

inline constexpr float kCameraNearPlane = 0.1f;

// Every consumer (rendering, picking, gizmos, annotations) must use this so the math agrees.
// Depth maps to [0, 1] as Vulkan expects.
inline glm::mat4 getProjection(float width, float height, float nearPlane, float farPlane) {
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), width / height, nearPlane, farPlane);
    proj[1][1] *= -1; // Vulkan clip space has Y pointing down
    return proj;
}

struct FrustumPlanes {
    glm::vec4 planes[6];
};

// Gribb-Hartmann extraction; planes are in the space the matrix maps from (e.g. model space for P*V*M).
inline FrustumPlanes extractFrustumPlanes(const glm::mat4& m) {
    glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
    glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
    glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
    glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);
    return { { r3 + r0, r3 - r0, r3 + r1, r3 - r1, r2, r3 - r2 } };
}

inline bool isAABBInFrustum(const FrustumPlanes& f, const glm::vec3& minB, const glm::vec3& maxB) {
    for (const glm::vec4& p : f.planes) {
        glm::vec3 positive(p.x >= 0.0f ? maxB.x : minB.x,
            p.y >= 0.0f ? maxB.y : minB.y,
            p.z >= 0.0f ? maxB.z : minB.z);
        if (p.x * positive.x + p.y * positive.y + p.z * positive.z + p.w < 0.0f)
            return false;
    }
    return true;
}
