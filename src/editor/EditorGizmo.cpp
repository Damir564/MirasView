#include "Editor.h"
#include <algorithm>
#include <cmath>
#include "engine/ModelManager.h"

namespace {

constexpr ImU32 kAxisColors[3] = {
    IM_COL32(222, 64, 52, 255),
    IM_COL32(120, 200, 50, 255),
    IM_COL32(56, 122, 232, 255),
};
constexpr ImU32 kGizmoHighlight = IM_COL32(255, 214, 40, 255);
constexpr ImU32 kOrientationNegative = IM_COL32(150, 150, 150, 255);

constexpr float kOrientationRadius = 42.0f;
constexpr float kOrientationMargin = 62.0f;
constexpr float kOrientationEndRadius = 9.0f;
constexpr float kOrientationNegativeRadius = 6.5f;
constexpr float kViewTurnSeconds = 0.25f;

ImVec2 toImVec(const glm::vec2& v, const ImVec2& origin)
{
    return ImVec2(origin.x + v.x, origin.y + v.y);
}

ImU32 withAlpha(ImU32 color, float alpha)
{
    const ImU32 a = static_cast<ImU32>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f);
    return (color & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

ImU32 scaleColor(ImU32 color, float factor)
{
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
    c.x = std::min(c.x * factor, 1.0f);
    c.y = std::min(c.y * factor, 1.0f);
    c.z = std::min(c.z * factor, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(c);
}

ImU32 lighten(ImU32 color, float amount)
{
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
    c.x += (1.0f - c.x) * amount;
    c.y += (1.0f - c.y) * amount;
    c.z += (1.0f - c.z) * amount;
    return ImGui::ColorConvertFloat4ToU32(c);
}

float cross2(const glm::vec2& o, const glm::vec2& a, const glm::vec2& b)
{
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

// Monotone chain; the projection of a convex solid is the hull of its projected vertices.
std::vector<glm::vec2> convexHull(std::vector<glm::vec2> points)
{
    std::sort(points.begin(), points.end(), [](const glm::vec2& a, const glm::vec2& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    if (points.size() < 3)
        return points;
    std::vector<glm::vec2> hull(points.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < points.size(); ++i) {
        while (k >= 2 && cross2(hull[k - 2], hull[k - 1], points[i]) <= 0.0f) --k;
        hull[k++] = points[i];
    }
    for (size_t i = points.size() - 1, lower = k + 1; i > 0; --i) {
        while (k >= lower && cross2(hull[k - 2], hull[k - 1], points[i - 1]) <= 0.0f) --k;
        hull[k++] = points[i - 1];
    }
    hull.resize(k - 1);
    return hull;
}

// Projects world points and fills their convex hull. Skipped if any point is behind the camera.
void fillProjectedHull(ImDrawList* drawList, const ImVec2& origin, const std::vector<glm::vec3>& world,
    const glm::mat4& viewProj, float width, float height, ImU32 fill, ImU32 outline)
{
    std::vector<glm::vec2> screen;
    screen.reserve(world.size());
    for (const glm::vec3& point : world) {
        const glm::vec2 s = worldToScreen(point, viewProj, width, height);
        if (s.x < -5000.0f)
            return;
        screen.push_back(s);
    }
    const std::vector<glm::vec2> hull = convexHull(std::move(screen));
    if (hull.size() < 3)
        return;
    std::vector<ImVec2> polygon;
    polygon.reserve(hull.size());
    for (const glm::vec2& p : hull) polygon.push_back(toImVec(p, origin));
    drawList->AddConvexPolyFilled(polygon.data(), static_cast<int>(polygon.size()), fill);
    if (outline != 0)
        drawList->AddPolyline(polygon.data(), static_cast<int>(polygon.size()), outline, ImDrawFlags_Closed, 1.0f);
}

void perpendicularBasis(const glm::vec3& dir, glm::vec3& u, glm::vec3& v)
{
    const glm::vec3 helper = std::abs(dir.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    u = glm::normalize(glm::cross(dir, helper));
    v = glm::cross(dir, u);
}

float shortestAngleDelta(float from, float to)
{
    float delta = std::fmod(to - from, 360.0f);
    if (delta > 180.0f) delta -= 360.0f;
    if (delta < -180.0f) delta += 360.0f;
    return delta;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Transform gizmo
// ---------------------------------------------------------------------------------------------

void Editor::drawTransformGizmo()
{
    const GizmoShape shape = currentGizmoShape();
    if (!shape.valid) {
        m_hoveredAxis = GizmoAxis::None;
        return;
    }

    const ImVec2 origin(m_sceneView.x, m_sceneView.y);
    if (!m_gizmo.isDragging) {
        const ImVec2 mouse = ImGui::GetMousePos();
        const bool mouseInView = sceneViewContains(mouse.x, mouse.y) && !ImGui::GetIO().WantCaptureMouse &&
            !m_rightMouseHeld;
        m_hoveredAxis = mouseInView
            ? pickGizmoShape(shape, glm::vec2(mouse.x - origin.x, mouse.y - origin.y), kGizmoPickRadius).axis
            : GizmoAxis::None;
    }
    const GizmoAxis highlighted = m_gizmo.isDragging ? m_gizmo.activeAxis : m_hoveredAxis;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->PushClipRect(origin, ImVec2(origin.x + m_sceneView.width, origin.y + m_sceneView.height), false);

    const glm::mat4 viewProj = sceneProjection() * getView(m_camera);
    const float w = m_sceneView.width;
    const float h = m_sceneView.height;

    // Far axes first so nearer handles overlap them.
    int order[3] = { 0, 1, 2 };
    std::sort(order, order + 3, [&](int a, int b) { return shape.axes[a].depth > shape.axes[b].depth; });

    for (int i : order) {
        const GizmoAxisShape& axisShape = shape.axes[i];
        if (!axisShape.projected)
            continue;
        const ImU32 color = axisShape.axis == highlighted ? kGizmoHighlight : kAxisColors[i];
        const glm::vec3 dir = gizmoAxisDirection(axisShape.axis);

        if (shape.mode == GizmoMode::Rotate) {
            for (size_t k = 0; k + 1 < axisShape.screen.size(); ++k) {
                const bool front = axisShape.front[k] && axisShape.front[k + 1];
                drawList->AddLine(toImVec(axisShape.screen[k], origin), toImVec(axisShape.screen[k + 1], origin),
                    front ? color : withAlpha(color, 0.25f), front ? 3.0f : 1.5f);
            }
            continue;
        }

        const glm::vec3 tip = axisShape.world[1];
        if (shape.mode == GizmoMode::Translate) {
            const glm::vec3 base = tip - dir * (kGizmoConeLength * shape.scale);
            const glm::vec2 baseScreen = worldToScreen(base, viewProj, w, h);
            if (baseScreen.x > -5000.0f)
                drawList->AddLine(toImVec(axisShape.screen[0], origin), toImVec(baseScreen, origin), color, 3.0f);
            glm::vec3 u, v;
            perpendicularBasis(dir, u, v);
            std::vector<glm::vec3> cone{ tip };
            const float radius = kGizmoConeRadius * shape.scale;
            for (int k = 0; k < 16; ++k) {
                const float angle = 2.0f * 3.14159265f * static_cast<float>(k) / 16.0f;
                cone.push_back(base + (u * std::cos(angle) + v * std::sin(angle)) * radius);
            }
            fillProjectedHull(drawList, origin, cone, viewProj, w, h, color, scaleColor(color, 0.6f));
        }
        else {
            const float half = kGizmoCubeHalfSize * shape.scale;
            const glm::vec2 lineEnd = worldToScreen(tip - dir * half, viewProj, w, h);
            if (lineEnd.x > -5000.0f)
                drawList->AddLine(toImVec(axisShape.screen[0], origin), toImVec(lineEnd, origin), color, 3.0f);
            std::vector<glm::vec3> cube;
            for (int corner = 0; corner < 8; ++corner)
                cube.push_back(tip + glm::vec3(corner & 1 ? half : -half, corner & 2 ? half : -half, corner & 4 ? half : -half));
            fillProjectedHull(drawList, origin, cube, viewProj, w, h, color, scaleColor(color, 0.6f));
        }
    }
    drawList->AddCircleFilled(toImVec(shape.screenCenter, origin), 3.5f, IM_COL32(230, 230, 230, 220));
    drawList->PopClipRect();
}

// ---------------------------------------------------------------------------------------------
// Scene orientation gizmo
// ---------------------------------------------------------------------------------------------

glm::vec2 Editor::orientationCenter() const
{
    return glm::vec2(m_sceneView.width - kOrientationMargin, kOrientationMargin);
}

std::array<Editor::OrientationHandle, 6> Editor::orientationHandles() const
{
    const glm::mat3 rotation(getView(m_camera));
    const glm::vec2 center = orientationCenter();
    std::array<OrientationHandle, 6> handles;
    for (int i = 0; i < 6; ++i) {
        OrientationHandle& handle = handles[i];
        handle.axis = i;
        handle.direction = gizmoAxisDirection(static_cast<GizmoAxis>(i % 3 + 1)) * (i < 3 ? 1.0f : -1.0f);
        const glm::vec3 viewDir = rotation * handle.direction;
        // View space has +Y up; the screen has +Y down.
        handle.position = center + glm::vec2(viewDir.x, -viewDir.y) * kOrientationRadius;
        handle.depth = viewDir.z;
    }
    std::sort(handles.begin(), handles.end(), [](const OrientationHandle& a, const OrientationHandle& b) {
        return a.depth < b.depth;
    });
    return handles;
}

int Editor::pickOrientationHandle(float x, float y) const
{
    if (m_sceneView.width < kOrientationMargin * 2.0f || m_sceneView.height < kOrientationMargin * 2.0f)
        return -1;
    const auto handles = orientationHandles();
    // Nearest handles are last and drawn on top, so they win.
    for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
        const float radius = it->axis < 3 ? kOrientationEndRadius : kOrientationNegativeRadius;
        if (glm::length(glm::vec2(x, y) - it->position) <= radius + 2.0f)
            return it->axis;
    }
    return -1;
}

void Editor::drawOrientationGizmo()
{
    if (m_sceneView.width < kOrientationMargin * 2.0f || m_sceneView.height < kOrientationMargin * 2.0f) {
        m_hoveredOrientation = -1;
        return;
    }
    const ImVec2 origin(m_sceneView.x, m_sceneView.y);
    const ImVec2 mouse = ImGui::GetMousePos();
    m_hoveredOrientation = (!ImGui::GetIO().WantCaptureMouse && !m_gizmo.isDragging && !m_rightMouseHeld)
        ? pickOrientationHandle(mouse.x - origin.x, mouse.y - origin.y) : -1;

    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->PushClipRect(origin, ImVec2(origin.x + m_sceneView.width, origin.y + m_sceneView.height), false);
    const ImVec2 center = toImVec(orientationCenter(), origin);
    drawList->AddCircleFilled(center, kOrientationRadius + 14.0f, IM_COL32(20, 22, 26, 70));

    static constexpr const char* kLabels[3] = { "X", "Y", "Z" };
    for (const OrientationHandle& handle : orientationHandles()) {
        const ImVec2 position = toImVec(handle.position, origin);
        const bool hovered = handle.axis == m_hoveredOrientation;
        if (handle.axis < 3) {
            ImU32 color = kAxisColors[handle.axis];
            if (hovered) color = lighten(color, 0.45f);
            drawList->AddLine(center, position, color, 2.5f);
            drawList->AddCircleFilled(position, kOrientationEndRadius, color);
            const ImVec2 textSize = ImGui::CalcTextSize(kLabels[handle.axis]);
            drawList->AddText(ImVec2(position.x - textSize.x * 0.5f, position.y - textSize.y * 0.5f),
                IM_COL32(20, 20, 20, 255), kLabels[handle.axis]);
        }
        else {
            const ImU32 color = hovered ? lighten(kOrientationNegative, 0.5f) : kOrientationNegative;
            drawList->AddLine(center, position, withAlpha(color, 0.5f), 1.5f);
            drawList->AddCircleFilled(position, kOrientationNegativeRadius, color);
        }
        if (hovered)
            drawList->AddCircle(position, (handle.axis < 3 ? kOrientationEndRadius : kOrientationNegativeRadius) + 1.5f,
                IM_COL32(255, 255, 255, 230), 0, 1.5f);
    }
    drawList->PopClipRect();

    if (m_hoveredOrientation >= 0) {
        static constexpr const char* kNames[6] = { "Right (+X)", "Top (+Y)", "Front (+Z)", "Left (-X)", "Bottom (-Y)", "Back (-Z)" };
        ImGui::SetTooltip("View from %s", kNames[m_hoveredOrientation]);
    }
}

// Clicking an axis end views the scene from that side: the camera ends up on that axis looking back.
void Editor::beginViewTurn(int axis)
{
    if (m_cameraAnimator.isPlaying())
        return;
    const glm::vec3 fromDir = gizmoAxisDirection(static_cast<GizmoAxis>(axis % 3 + 1)) * (axis < 3 ? 1.0f : -1.0f);
    const glm::vec3 front = -fromDir;

    ViewTurn turn;
    turn.active = true;
    turn.startPosition = m_camera.position;
    turn.startYaw = m_camera.yaw;
    turn.startPitch = m_camera.pitch;
    if (hasSelection()) {
        turn.pivot = instanceWorldCenter(m_gizmo.selectedInstance);
        turn.distance = std::max(glm::length(m_camera.position - turn.pivot), 1.0f);
    }
    else {
        turn.distance = 10.0f;
        turn.pivot = m_camera.position + getFront(m_camera) * turn.distance;
    }
    if (std::abs(front.y) > 0.999f) {
        // Straight up/down: yaw is free; keep the current one so the view does not spin.
        turn.targetYaw = m_camera.yaw;
        turn.targetPitch = front.y > 0.0f ? 89.0f : -89.0f;
    }
    else {
        turn.targetYaw = glm::degrees(std::atan2(front.z, front.x));
        turn.targetPitch = glm::degrees(std::asin(glm::clamp(front.y, -1.0f, 1.0f)));
    }
    m_viewTurn = turn;
}

void Editor::updateViewTurn(float dt)
{
    if (!m_viewTurn.active)
        return;
    if (m_cameraAnimator.isPlaying()) {
        m_viewTurn.active = false;
        return;
    }
    m_viewTurn.elapsed += dt;
    const float t = glm::clamp(m_viewTurn.elapsed / kViewTurnSeconds, 0.0f, 1.0f);
    const float s = t * t * (3.0f - 2.0f * t);

    m_camera.yaw = m_viewTurn.startYaw + shortestAngleDelta(m_viewTurn.startYaw, m_viewTurn.targetYaw) * s;
    m_camera.pitch = glm::mix(m_viewTurn.startPitch, m_viewTurn.targetPitch, s);
    // Orbit around the pivot; blend from the start position so the first frame does not jump.
    const glm::vec3 orbit = m_viewTurn.pivot - getFront(m_camera) * m_viewTurn.distance;
    m_camera.position = glm::mix(m_viewTurn.startPosition, orbit, s);
    if (t >= 1.0f) {
        m_camera.yaw = std::fmod(m_camera.yaw, 360.0f);
        m_viewTurn.active = false;
    }
}
