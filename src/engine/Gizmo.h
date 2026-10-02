#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>
#include <string>
#include "ModelManager.h"

enum class GizmoMode {
    None = 0,
    Translate = 1,
    Rotate = 2,
    Scale = 3
};

enum class GizmoAxis {
    None = 0,
    X = 1,
    Y = 2,
    Z = 3
};

struct Gizmo {
    int selectedInstance = -1;
    GizmoMode mode = GizmoMode::None;
    GizmoAxis activeAxis = GizmoAxis::None;
    bool isDragging = false;
    glm::vec2 dragStart{ 0.0f };
    glm::vec3 originalPosition{ 0.0f };
    glm::vec3 originalRotation{ 0.0f };
    glm::vec3 originalScale{ 1.0f };
    // Unit screen direction that counts as positive drag (the axis, or the ring tangent for rotation).
    glm::vec2 dragDirection{ 1.0f, 0.0f };
    // Screen pixels per world unit along the dragged axis (translation only).
    float pixelsPerUnit = 1.0f;

    void select(int instanceIndex) {
        selectedInstance = instanceIndex;
        if (instanceIndex >= 0 && mode == GizmoMode::None) {
            mode = GizmoMode::Translate;
        }
    }

    void deselect() {
        selectedInstance = -1;
        mode = GizmoMode::None;
        activeAxis = GizmoAxis::None;
        isDragging = false;
    }
};

struct Ray {
    glm::vec3 origin;
    glm::vec3 direction;
};

// ============================================================
// Core coordinate conversion functions
// All functions use the SAME convention:
//   - Projection has Y flipped (proj[1][1] *= -1 for Vulkan)
//   - Screen: (0,0) = top-left, Y increases downward
//   - NDC after Vulkan flip: Y increases downward (matches screen)
// ============================================================

// Screen pixel coords -> NDC (accounting for Vulkan Y-flip in projection)
// Since proj already flips Y, NDC Y increases downward just like screen Y.
// So the mapping is simply: ndcX = 2*mx/w - 1, ndcY = 2*my/h - 1
inline glm::vec2 screenToNDC(float mouseX, float mouseY,
    float screenWidth, float screenHeight)
{
    return glm::vec2(
        (2.0f * mouseX) / screenWidth - 1.0f,
        (2.0f * mouseY) / screenHeight - 1.0f  // No flip needed - Vulkan proj already flips
    );
}

inline Ray screenToWorldRay(float mouseX, float mouseY,
    float screenWidth, float screenHeight,
    const glm::mat4& view, const glm::mat4& proj)
{
    glm::vec2 ndc = screenToNDC(mouseX, mouseY, screenWidth, screenHeight);

    glm::vec4 clipNear(ndc.x, ndc.y, 0.0f, 1.0f);
    glm::vec4 clipFar(ndc.x, ndc.y, 1.0f, 1.0f);

    glm::mat4 invVP = glm::inverse(proj * view);
    glm::vec4 worldNear = invVP * clipNear;
    glm::vec4 worldFar = invVP * clipFar;
    worldNear /= worldNear.w;
    worldFar /= worldFar.w;

    Ray ray;
    ray.origin = glm::vec3(worldNear);
    ray.direction = glm::normalize(glm::vec3(worldFar - worldNear));
    return ray;
}

// World position -> screen pixel coords
// Since proj already flips Y, NDC Y matches screen Y direction
inline glm::vec2 worldToScreen(const glm::vec3& worldPos,
    const glm::mat4& vp,
    float screenWidth, float screenHeight)
{
    glm::vec4 clip = vp * glm::vec4(worldPos, 1.0f);
    if (clip.w <= 0.0001f) return glm::vec2(-10000.0f);
    glm::vec3 ndc = glm::vec3(clip) / clip.w;

    // Direct mapping - no Y flip because proj already flipped Y
    return glm::vec2(
        (ndc.x * 0.5f + 0.5f) * screenWidth,
        (ndc.y * 0.5f + 0.5f) * screenHeight
    );
}

inline bool rayIntersectsAABB(const Ray& ray,
    const glm::vec3& boundsMin, const glm::vec3& boundsMax,
    float& tOut)
{
    float tmin = -1e30f;
    float tmax = 1e30f;

    for (int i = 0; i < 3; ++i) {
        if (std::abs(ray.direction[i]) < 1e-8f) {
            if (ray.origin[i] < boundsMin[i] || ray.origin[i] > boundsMax[i])
                return false;
        }
        else {
            float invD = 1.0f / ray.direction[i];
            float t1 = (boundsMin[i] - ray.origin[i]) * invD;
            float t2 = (boundsMax[i] - ray.origin[i]) * invD;

            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);

            if (tmin > tmax)
                return false;
        }
    }

    if (tmax < 0.0f)
        return false;
    // Entry distance; 0 when the ray starts inside, so it never hides closer geometry.
    tOut = std::max(tmin, 0.0f);
    return true;
}

// Two-sided Moller-Trumbore. t is in units of ray.direction, which need not be normalized.
inline bool rayIntersectsTriangle(const Ray& ray,
    const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2,
    float& tOut)
{
    const glm::vec3 e1 = v1 - v0;
    const glm::vec3 e2 = v2 - v0;
    const glm::vec3 p = glm::cross(ray.direction, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-12f)
        return false;
    const float invDet = 1.0f / det;
    const glm::vec3 s = ray.origin - v0;
    const float u = glm::dot(s, p) * invDet;
    if (u < 0.0f || u > 1.0f)
        return false;
    const glm::vec3 q = glm::cross(s, e1);
    const float v = glm::dot(ray.direction, q) * invDet;
    if (v < 0.0f || u + v > 1.0f)
        return false;
    const float t = glm::dot(e2, q) * invDet;
    if (t <= 0.0f)
        return false;
    tOut = t;
    return true;
}

// Nearest triangle hit of a submesh, or false. Bounds only reject; they are never reported as a hit.
inline bool rayIntersectsSubmesh(const Ray& ray, const GPUModel& model, const SubmeshInfo& sub, float maxT, float& tOut)
{
    const bool validBounds = sub.boundsMin.x <= sub.boundsMax.x;
    float boxT;
    if (validBounds && (!rayIntersectsAABB(ray, sub.boundsMin, sub.boundsMax, boxT) || boxT >= maxT))
        return false;

    const size_t end = std::min<size_t>(size_t(sub.indexOffset) + sub.indexCount, model.indices.size());
    bool found = false;
    float best = maxT;
    for (size_t i = sub.indexOffset; i + 2 < end; i += 3) {
        const size_t a = size_t(model.indices[i]) + sub.vertexOffset;
        const size_t b = size_t(model.indices[i + 1]) + sub.vertexOffset;
        const size_t c = size_t(model.indices[i + 2]) + sub.vertexOffset;
        if (a >= model.positions.size() || b >= model.positions.size() || c >= model.positions.size())
            continue;
        float t;
        if (rayIntersectsTriangle(ray, model.positions[a], model.positions[b], model.positions[c], t) && t < best) {
            best = t;
            found = true;
        }
    }
    if (found)
        tOut = best;
    return found;
}

struct SubmeshHitResult {
    int   instanceIndex = -1;
    size_t submeshIndex = std::numeric_limits<size_t>::max();
    float t = std::numeric_limits<float>::max();
    std::string ifcGuid;                  

    bool hit() const { return instanceIndex >= 0; }
};

template<typename GetModelFn>
inline SubmeshHitResult pickSubmesh(
    const Ray& ray,
    const std::vector<ModelInstance>& instances,
    GetModelFn&& getModel)
{
    SubmeshHitResult best;

    for (size_t i = 0; i < instances.size(); ++i) {
        const ModelInstance& inst = instances[i];
        if (!inst.visible) continue;

        GPUModel* model = getModel(inst.modelIndex);
        if (!model || !model->isValid()) continue;

        // The local direction is deliberately left unnormalized: an affine map keeps the ray parameter,
        // so local t equals world t and hits from differently scaled instances compare correctly.
        const glm::mat4 invTransform = glm::inverse(inst.getTransformMatrix());
        Ray localRay;
        localRay.origin = glm::vec3(invTransform * glm::vec4(ray.origin, 1.0f));
        localRay.direction = glm::vec3(invTransform * glm::vec4(ray.direction, 0.0f));

        float modelT;
        if (!rayIntersectsAABB(localRay, model->boundsMin, model->boundsMax, modelT) || modelT >= best.t)
            continue;

        const bool hasIfc = inst.ifcScene.has_value();

        for (size_t si = 0; si < model->submeshes.size(); ++si) {

            if (hasIfc && !inst.ifcScene->isSubmeshVisible(si))
                continue;

            const SubmeshInfo& sub = model->submeshes[si];

            float subT;
            if (!rayIntersectsSubmesh(localRay, *model, sub, best.t, subT))
                continue;

            if (subT < best.t) {
                best.t = subT;
                best.instanceIndex = static_cast<int>(i);
                best.submeshIndex = si;

                best.ifcGuid.clear();
                if (hasIfc) {
                    auto it = inst.ifcScene->submeshToGuid.find(si);
                    if (it != inst.ifcScene->submeshToGuid.end())
                        best.ifcGuid = it->second;
                }
            }
        }
    }

    return best;
}

inline float pointToSegment2D(const glm::vec2& point,
    const glm::vec2& segA, const glm::vec2& segB,
    float& segT)
{
    glm::vec2 ab = segB - segA;
    float abLen2 = glm::dot(ab, ab);

    if (abLen2 < 0.0001f) {
        segT = 0.0f;
        return glm::length(point - segA);
    }

    segT = glm::clamp(glm::dot(point - segA, ab) / abLen2, 0.0f, 1.0f);
    glm::vec2 closest = segA + ab * segT;
    return glm::length(point - closest);
}

inline float getGizmoScale(const glm::vec3& gizmoPos, const glm::vec3& cameraPos,
    float desiredScreenSize = 0.15f,
    const glm::mat4& proj = glm::mat4(1.0f))
{
    float dist = glm::length(gizmoPos - cameraPos);
    // proj[1][1] is negative due to Vulkan flip, use abs
    float tanHalfFov = 1.0f / std::abs(proj[1][1]);
    return dist * tanHalfFov * desiredScreenSize;
}

inline glm::vec3 gizmoAxisDirection(GizmoAxis axis)
{
    switch (axis) {
    case GizmoAxis::X: return { 1.0f, 0.0f, 0.0f };
    case GizmoAxis::Y: return { 0.0f, 1.0f, 0.0f };
    case GizmoAxis::Z: return { 0.0f, 0.0f, 1.0f };
    default: return glm::vec3(0.0f);
    }
}

// Transform gizmo geometry in world units of gizmoScale. Drawing and picking both go through
// buildGizmoShape() so that what is clickable is exactly what is drawn.
inline constexpr float kGizmoAxisLength = 2.0f;
inline constexpr float kGizmoRingRadius = 1.5f;
inline constexpr float kGizmoConeLength = 0.4f;
inline constexpr float kGizmoConeRadius = 0.12f;
inline constexpr float kGizmoCubeHalfSize = 0.12f;
inline constexpr int kGizmoRingSegments = 64;

struct GizmoAxisShape {
    GizmoAxis axis = GizmoAxis::None;
    // Move/scale: {center, tip}. Rotate: closed ring (first point repeated at the end).
    std::vector<glm::vec3> world;
    std::vector<glm::vec2> screen;
    // Per ring point: faces the camera. Only the front half of a ring is pickable.
    std::vector<uint8_t> front;
    bool projected = false;
    // View-space distance of the axis tip; larger = farther, drawn first.
    float depth = 0.0f;
};

struct GizmoShape {
    bool valid = false;
    GizmoMode mode = GizmoMode::None;
    glm::vec3 center{ 0.0f };
    float scale = 1.0f;
    glm::vec2 screenCenter{ 0.0f };
    GizmoAxisShape axes[3];
};

inline GizmoShape buildGizmoShape(GizmoMode mode, const glm::vec3& center, const glm::vec3& cameraPos,
    const glm::mat4& view, const glm::mat4& proj, float screenWidth, float screenHeight)
{
    GizmoShape shape;
    shape.mode = mode;
    shape.center = center;
    const glm::mat4 vp = proj * view;
    if (mode == GizmoMode::None || (vp * glm::vec4(center, 1.0f)).w <= 0.0001f)
        return shape;
    shape.scale = getGizmoScale(center, cameraPos, 0.15f, proj);
    shape.screenCenter = worldToScreen(center, vp, screenWidth, screenHeight);
    const glm::vec3 toCamera = cameraPos - center;

    for (int i = 0; i < 3; ++i) {
        GizmoAxisShape& axisShape = shape.axes[i];
        axisShape.axis = static_cast<GizmoAxis>(i + 1);
        const glm::vec3 dir = gizmoAxisDirection(axisShape.axis);
        if (mode == GizmoMode::Rotate) {
            const glm::vec3 u = gizmoAxisDirection(static_cast<GizmoAxis>((i + 1) % 3 + 1));
            const glm::vec3 v = glm::cross(dir, u);
            const float radius = kGizmoRingRadius * shape.scale;
            for (int k = 0; k <= kGizmoRingSegments; ++k) {
                const float angle = 2.0f * 3.14159265f * static_cast<float>(k) / kGizmoRingSegments;
                const glm::vec3 offset = (u * std::cos(angle) + v * std::sin(angle)) * radius;
                axisShape.world.push_back(center + offset);
                axisShape.front.push_back(glm::dot(offset, toCamera) >= -0.02f * radius * glm::length(toCamera) ? 1 : 0);
            }
        }
        else {
            axisShape.world = { center, center + dir * (kGizmoAxisLength * shape.scale) };
        }
        axisShape.projected = true;
        for (const glm::vec3& point : axisShape.world) {
            const glm::vec2 screenPoint = worldToScreen(point, vp, screenWidth, screenHeight);
            if (screenPoint.x < -5000.0f) axisShape.projected = false;
            axisShape.screen.push_back(screenPoint);
        }
        axisShape.depth = -(view * glm::vec4(center + dir * shape.scale, 1.0f)).z;
    }
    shape.valid = true;
    return shape;
}

struct GizmoPick {
    GizmoAxis axis = GizmoAxis::None;
    // Rotate: the ring point under the mouse (drag direction follows the ring's tangent there).
    glm::vec3 worldPoint{ 0.0f };
};

inline GizmoPick pickGizmoShape(const GizmoShape& shape, const glm::vec2& mouse, float pickRadiusPixels)
{
    GizmoPick best;
    if (!shape.valid)
        return best;
    float bestDist = pickRadiusPixels;
    for (const GizmoAxisShape& axisShape : shape.axes) {
        if (!axisShape.projected)
            continue;
        const size_t count = axisShape.screen.size();
        for (size_t k = 0; k + 1 < count; ++k) {
            if (shape.mode == GizmoMode::Rotate && !(axisShape.front[k] && axisShape.front[k + 1]))
                continue;
            glm::vec2 a = axisShape.screen[k];
            glm::vec2 b = axisShape.screen[k + 1];
            if (shape.mode != GizmoMode::Rotate && glm::length(b - a) < 2.0f)
                continue;
            float t = 0.0f;
            const float dist = pointToSegment2D(mouse, a, b, t);
            if (dist < bestDist) {
                bestDist = dist;
                best.axis = axisShape.axis;
                best.worldPoint = glm::mix(axisShape.world[k], axisShape.world[k + 1], t);
            }
        }
    }
    return best;
}

struct GizmoVertex {
    glm::vec3 position;
    glm::vec3 color;
};
