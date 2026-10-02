#include "Editor.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include "engine/ModelLoader.h"
#include "engine/ModelManager.h"
#include "engine/SceneManager.h"
#include "engine/Log.h"

Editor::Editor(const EngineContext& engine)
    : m_window(engine.window)
    , m_vulkan(engine.vulkan)
    , m_renderer(engine.renderer)
    , m_models(engine.models)
    , m_scenes(engine.scenes)
    , m_settings(engine.settings)
{
    int width = 0, height = 0;
    SDL_GetWindowSize(m_window, &width, &height);
    m_sceneView = { 0.0f, 0.0f, static_cast<float>(std::max(width, 1)), static_cast<float>(std::max(height, 1)) };

    m_cameraAnimator.getPath().name = m_pathName;
    LOG_INFO("Camera animation system initialized\n");

    SDL_SetWindowRelativeMouseMode(m_window, m_flyMode);
}

// ---------------------------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------------------------

void Editor::onEvent(const SDL_Event& event)
{
    if (m_flyMode || !ImGui::GetIO().WantCaptureKeyboard) {
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
            handleKeyDown(event.key);
        if (event.type == SDL_EVENT_KEY_UP && !event.key.repeat && !(event.key.mod & SDL_KMOD_SHIFT))
            m_cameraSpeedMultiplier = 1.0f;
    }
    if (!m_flyMode)
        handleViewportMouse(event);
    handleCameraLook(event);
}

void Editor::handleKeyDown(const SDL_KeyboardEvent& key)
{
    // Escape leaves fly mode (and deselects via the UI shortcut); quitting is File > Exit or closing the window.
    if (key.scancode == SDL_SCANCODE_ESCAPE && m_flyMode)
        setFlyMode(false);

    if (key.mod & SDL_KMOD_SHIFT) {
        m_cameraSpeedMultiplier = 4.0f;
        if (key.scancode == SDL_SCANCODE_GRAVE)
            setFlyMode(!m_flyMode);
    }

    dropStaleGizmoSelection();

    if (!m_flyMode && key.scancode == SDL_SCANCODE_M && !m_selectedIfcGuid.empty())
        requestAnnotation();
}

void Editor::dropStaleGizmoSelection()
{
    if (m_gizmo.selectedInstance < 0)
        return;
    const auto& instances = m_models.getInstances();
    if (m_gizmo.selectedInstance >= static_cast<int>(instances.size())) {
        m_gizmo.deselect();
        return;
    }
    GPUModel* model = m_models.getModel(instances[m_gizmo.selectedInstance].modelIndex);
    if (!model || !model->isValid())
        m_gizmo.deselect();
}

void Editor::handleViewportMouse(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT &&
        !ImGui::GetIO().WantCaptureMouse && sceneViewContains(event.button.x, event.button.y))
        handleViewportClick(event.button.x - m_sceneView.x, event.button.y - m_sceneView.y);

    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
        m_gizmo.isDragging = false;
        m_gizmo.activeAxis = GizmoAxis::None;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION && m_gizmo.isDragging && validInstance(m_gizmo.selectedInstance))
        dragGizmo(event.motion.x - m_sceneView.x, event.motion.y - m_sceneView.y);
}

void Editor::handleViewportClick(float mouseX, float mouseY)
{
    const int orientationAxis = pickOrientationHandle(mouseX, mouseY);
    if (orientationAxis >= 0) {
        beginViewTurn(orientationAxis);
        return;
    }

    const glm::mat4 view = getView(m_camera);
    const glm::mat4 proj = sceneProjection();
    if (tryBeginGizmoDrag(mouseX, mouseY, view, proj))
        return;

    const Ray ray = screenToWorldRay(mouseX, mouseY, m_sceneView.width, m_sceneView.height, view, proj);
    const SubmeshHitResult hit = pickSubmesh(ray, m_models.getInstances(),
        [&](size_t index) { return m_models.getModel(index); });
    if (hit.hit()) {
        selectPickedSubmesh(hit);
    }
    else {
        clearIfcSelection();
        m_gizmo.deselect();
    }
}

bool Editor::gizmoVisible() const
{
    return !m_flyMode && hasSelection() && m_gizmo.mode != GizmoMode::None &&
        m_models.getInstances()[m_gizmo.selectedInstance].visible;
}

GizmoShape Editor::currentGizmoShape() const
{
    if (!gizmoVisible())
        return {};
    return buildGizmoShape(m_gizmo.mode, m_models.getInstances()[m_gizmo.selectedInstance].position,
        m_camera.position, getView(m_camera), sceneProjection(), m_sceneView.width, m_sceneView.height);
}

bool Editor::tryBeginGizmoDrag(float mouseX, float mouseY, const glm::mat4& view, const glm::mat4& proj)
{
    const GizmoShape shape = currentGizmoShape();
    const glm::vec2 mouse(mouseX, mouseY);
    const GizmoPick pick = pickGizmoShape(shape, mouse, kGizmoPickRadius);
    if (pick.axis == GizmoAxis::None)
        return false;

    auto& instance = m_models.getInstances()[m_gizmo.selectedInstance];
    const glm::mat4 viewProj = proj * view;
    const glm::vec3 axisDir = gizmoAxisDirection(pick.axis);
    glm::vec2 screenDir(1.0f, 0.0f);
    float pixelsPerUnit = 1.0f;
    if (m_gizmo.mode == GizmoMode::Rotate) {
        // Dragging along the ring's tangent at the grabbed point turns the object the way the ring moves.
        const glm::vec3 tangent = glm::normalize(glm::cross(axisDir, pick.worldPoint - shape.center));
        const glm::vec2 a = worldToScreen(pick.worldPoint, viewProj, m_sceneView.width, m_sceneView.height);
        const glm::vec2 b = worldToScreen(pick.worldPoint + tangent * (0.1f * shape.scale), viewProj,
            m_sceneView.width, m_sceneView.height);
        if (glm::length(b - a) > 0.001f && a.x > -5000.0f && b.x > -5000.0f)
            screenDir = glm::normalize(b - a);
    }
    else {
        // Mouse motion counts only along the axis as it appears on screen.
        const glm::vec2 a = shape.screenCenter;
        const glm::vec2 b = worldToScreen(shape.center + axisDir * shape.scale, viewProj,
            m_sceneView.width, m_sceneView.height);
        const float length = glm::length(b - a);
        if (length >= 1.0f && b.x > -5000.0f) {
            screenDir = (b - a) / length;
            pixelsPerUnit = length / shape.scale;
        }
    }

    m_gizmo.activeAxis = pick.axis;
    m_gizmo.isDragging = true;
    m_gizmo.dragStart = mouse;
    m_gizmo.dragDirection = screenDir;
    m_gizmo.pixelsPerUnit = pixelsPerUnit;
    m_gizmo.originalPosition = instance.position;
    m_gizmo.originalRotation = instance.rotation;
    m_gizmo.originalScale = instance.scale;
    return true;
}

void Editor::selectPickedSubmesh(const SubmeshHitResult& hit)
{
    clearIfcSelection();
    m_gizmo.select(hit.instanceIndex);

    auto& instance = m_models.getInstances()[hit.instanceIndex];
    if (!instance.ifcScene.has_value()) {
        LOG_INFO("[PICK] Instance " << hit.instanceIndex << " | Submesh " << hit.submeshIndex
            << " | t: " << hit.t << "\n");
        return;
    }

    IfcScene& scene = instance.ifcScene.value();
    for (auto& [guid, element] : scene.elements) element.selected = false;
    for (auto& [guid, node] : scene.spatial) node.selected = false;

    if (hit.ifcGuid.empty()) {
        LOG_INFO("[PICK] IFC model hit but no GUID for submesh " << hit.submeshIndex << "\n");
        return;
    }
    auto it = scene.elements.find(hit.ifcGuid);
    if (it == scene.elements.end())
        return;

    it->second.selected = true;
    m_selectedIfcGuid = hit.ifcGuid;
    m_ifcSelectionKind = IfcSelectionKind::Element;
    m_ifcSelectionInstance = hit.instanceIndex;
    m_selectionChangedFromViewport = true;
    LOG_INFO("[PICK] IFC Hit"
        << " | Type: " << it->second.type
        << " | Name: " << it->second.name
        << " | GUID: " << hit.ifcGuid
        << " | Submesh: " << hit.submeshIndex
        << " | t: " << hit.t
        << "\n");
}

void Editor::dragGizmo(float mouseX, float mouseY)
{
    auto& instance = m_models.getInstances()[m_gizmo.selectedInstance];
    const float amount = glm::dot(glm::vec2(mouseX, mouseY) - m_gizmo.dragStart, m_gizmo.dragDirection);
    const int axis = static_cast<int>(m_gizmo.activeAxis) - 1;
    if (axis < 0 || axis > 2)
        return;
    const glm::vec3 axisDir = gizmoAxisDirection(m_gizmo.activeAxis);
    const bool snap = (SDL_GetModState() & SDL_KMOD_CTRL) != 0;
    const auto snapTo = [](float value, float step) { return step > 0.0f ? std::round(value / step) * step : value; };

    switch (m_gizmo.mode) {
    case GizmoMode::Translate:
        instance.position = m_gizmo.originalPosition + axisDir * (amount / m_gizmo.pixelsPerUnit);
        if (snap)
            instance.position[axis] = snapTo(instance.position[axis], m_snapTranslate);
        break;
    case GizmoMode::Rotate: {
        const float degreesPerPixel = 0.5f;
        float degrees = amount * degreesPerPixel;
        if (snap) degrees = snapTo(degrees, m_snapRotate);
        instance.rotation = m_gizmo.originalRotation + axisDir * degrees;
        break;
    }
    case GizmoMode::Scale: {
        const float scalePerPixel = 0.01f;
        float delta = amount * scalePerPixel;
        if (snap) delta = snapTo(delta, m_snapScale);
        instance.scale = glm::max(m_gizmo.originalScale + axisDir * delta, glm::vec3(0.01f));
        break;
    }
    default:
        break;
    }
}

void Editor::handleCameraLook(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_RIGHT &&
        !m_flyMode && !ImGui::GetIO().WantCaptureMouse && sceneViewContains(event.button.x, event.button.y))
        m_rightMouseHeld = true;
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT)
        m_rightMouseHeld = false;

    if (event.type == SDL_EVENT_MOUSE_MOTION && !m_cameraAnimator.isPlaying() && (m_flyMode || m_rightMouseHeld)) {
        m_camera.yaw += event.motion.xrel * m_camera.sensitivity;
        m_camera.pitch -= event.motion.yrel * m_camera.sensitivity;
        m_camera.pitch = glm::clamp(m_camera.pitch, -89.0f, 89.0f);
    }
}

void Editor::moveCamera(float dt)
{
    if ((!m_flyMode && ImGui::GetIO().WantCaptureKeyboard) || m_cameraAnimator.isPlaying())
        return;
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const glm::vec3 front = getFront(m_camera);
    const glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0, 1, 0)));
    const float step = m_camera.speed * dt * m_cameraSpeedMultiplier;
    if (keys[SDL_SCANCODE_W]) m_camera.position += front * step;
    if (keys[SDL_SCANCODE_A]) m_camera.position -= right * step;
    if (keys[SDL_SCANCODE_D]) m_camera.position += right * step;
    if (keys[SDL_SCANCODE_S]) m_camera.position -= front * step;
}

void Editor::setFlyMode(bool enabled)
{
    m_flyMode = enabled;
    SDL_SetWindowRelativeMouseMode(m_window, enabled);
}

bool Editor::sceneViewContains(float x, float y) const
{
    return x >= m_sceneView.x && x < m_sceneView.x + m_sceneView.width &&
        y >= m_sceneView.y && y < m_sceneView.y + m_sceneView.height;
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------

void Editor::update(float dt)
{
    if (m_flyMode) {
        // Fly mode hides the editor UI, so the scene uses the whole window.
        int width = 0, height = 0;
        SDL_GetWindowSize(m_window, &width, &height);
        m_sceneView = { 0.0f, 0.0f, static_cast<float>(std::max(width, 1)), static_cast<float>(std::max(height, 1)) };
    }
    moveCamera(dt);
    updateViewTurn(dt);
}

void Editor::lateUpdate(float dt)
{
    if (m_cameraAnimator.update(dt)) {
        const CameraState state = m_cameraAnimator.getCurrentState();
        m_camera.position = state.position;
        m_camera.yaw = state.yaw;
        m_camera.pitch = state.pitch;
    }
}

void Editor::drawUi()
{
    validateSelection();
    handleShortcuts();
    // The gizmo shows the active tool on the selected object ("Select" tool = no gizmo).
    m_gizmo.mode = hasSelection() ? m_tool : GizmoMode::None;
    updateWindowTitle();

    drawMainMenuBar();
    drawToolbar();
    drawStatusBar();
    drawDockSpace();
    drawViewportOverlay();
    drawTransformGizmo();
    drawOrientationGizmo();
    if (m_showHierarchy) drawHierarchy();
    if (m_showInspector) drawInspector();
    drawAnnotationPopup();
    if (m_showAnnotations && !m_annotations.empty()) drawViewportAnnotations();
    if (m_showAnnotationsPanel) drawAnnotationsPanel();
    if (m_showStatisticsPanel) drawStatisticsPanel();
    if (m_showAnimationPanel) drawAnimationPanel();
    if (m_showGraphicsSettings) drawGraphicsSettingsWindow();
    drawFileDialogs();
    drawHelpPopups();
}

void Editor::fillFrame(FrameInput& frame)
{
    frame.models = &m_models;
    frame.view = getView(m_camera);
    frame.proj = sceneProjection();
    frame.cameraPosition = m_camera.position;
    frame.viewport = m_sceneView;
    fillHighlight(frame.highlight);
    frame.showPath = !m_flyMode && m_showCameraPath;
    frame.showGrid = !m_flyMode && m_showGrid;
}

glm::mat4 Editor::sceneProjection() const
{
    return getProjection(m_sceneView.width, m_sceneView.height, kCameraNearPlane, m_settings.viewDistance);
}

// IFC element -> its submesh; IFC spatial node -> every element below it; anything else -> the whole instance.
void Editor::fillHighlight(SelectionHighlight& highlight) const
{
    highlight = {};
    if (m_flyMode || !hasSelection())
        return;
    highlight.instance = m_gizmo.selectedInstance;

    const ModelInstance& instance = m_models.getInstances()[m_gizmo.selectedInstance];
    if (m_ifcSelectionKind == IfcSelectionKind::None || m_ifcSelectionInstance != m_gizmo.selectedInstance ||
        !instance.ifcScene)
        return;

    const IfcScene& scene = *instance.ifcScene;
    highlight.wholeInstance = false;
    if (m_ifcSelectionKind == IfcSelectionKind::Element) {
        for (size_t si : scene.submeshesOf(m_selectedIfcGuid))
            highlight.submeshes.push_back(static_cast<uint32_t>(si));
    }
    else {
        collectSpatialSubmeshes(scene, m_selectedIfcGuid, highlight.submeshes);
    }
}

void Editor::collectSpatialSubmeshes(const IfcScene& scene, const std::string& spatialGuid, std::vector<uint32_t>& out)
{
    const auto it = scene.spatial.find(spatialGuid);
    if (it == scene.spatial.end())
        return;
    for (const std::string& elementGuid : it->second.elementGuids) {
        for (size_t si : scene.submeshesOf(elementGuid))
            out.push_back(static_cast<uint32_t>(si));
    }
    for (const std::string& childGuid : it->second.childSpatialGuids)
        collectSpatialSubmeshes(scene, childGuid, out);
}

void Editor::handleShortcuts()
{
    if (ImGui::GetIO().WantTextInput)
        return;
    const bool selection = hasSelection();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) openSceneDialog();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) saveSceneAsDialog();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) saveScene();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_I)) importModelDialog();
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D) && selection) duplicateInstance(m_gizmo.selectedInstance);
    if (ImGui::IsKeyChordPressed(ImGuiKey_Delete) && selection) deleteInstance(m_gizmo.selectedInstance);
    if (ImGui::IsKeyChordPressed(ImGuiKey_F) && selection) focusOnInstance(m_gizmo.selectedInstance);
    if (ImGui::IsKeyChordPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) deselectAll();
    if (ImGui::IsKeyChordPressed(ImGuiKey_Q)) m_tool = GizmoMode::None;
    if (ImGui::IsKeyChordPressed(ImGuiKey_1)) m_tool = GizmoMode::Translate;
    if (ImGui::IsKeyChordPressed(ImGuiKey_2)) m_tool = GizmoMode::Rotate;
    if (ImGui::IsKeyChordPressed(ImGuiKey_3)) m_tool = GizmoMode::Scale;
    if (ImGui::IsKeyChordPressed(ImGuiKey_F1)) m_openControlsPopup = true;
    if (ImGui::IsKeyChordPressed(ImGuiKey_F2) && selection) beginRename(m_gizmo.selectedInstance);
}

void Editor::updateWindowTitle()
{
    const std::string& path = m_scenes.currentPath();
    std::string title = "MirasEngine - " + (path.empty() ? std::string("Untitled") : path);
    if (title != m_windowTitle) {
        SDL_SetWindowTitle(m_window, title.c_str());
        m_windowTitle = std::move(title);
    }
}

void Editor::setStatus(const std::string& message, bool isError)
{
    m_statusMessage = message;
    m_statusIsError = isError;
    m_statusTime = ImGui::GetTime();
    if (isError)
        LOG_ERROR("[EDITOR] " << message << "\n");
    else
        LOG_INFO("[EDITOR] " << message << "\n");
}

std::string Editor::formatCount(size_t value)
{
    const std::string digits = std::to_string(value);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

bool Editor::ifcElementWorldBounds(const GPUModel* model, const IfcScene& scene, const std::string& guid,
    const ModelInstance& instance, glm::vec3& center, float& radius)
{
    if (!model)
        return false;
    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
    for (size_t si : scene.submeshesOf(guid)) {
        if (si >= model->submeshes.size())
            continue;
        const auto& sub = model->submeshes[si];
        // Inverted bounds mean the loader never set them.
        if (sub.boundsMin.x > sub.boundsMax.x)
            continue;
        boundsMin = glm::min(boundsMin, sub.boundsMin);
        boundsMax = glm::max(boundsMax, sub.boundsMax);
    }
    if (boundsMin.x > boundsMax.x)
        return false;
    const glm::vec3 localCenter = (boundsMin + boundsMax) * 0.5f;
    center = glm::vec3(instance.getTransformMatrix() * glm::vec4(localCenter, 1.0f));
    const float maxScale = std::max({ instance.scale.x, instance.scale.y, instance.scale.z });
    radius = glm::length(boundsMax - boundsMin) * 0.5f * maxScale;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Selection
// ---------------------------------------------------------------------------------------------

bool Editor::validInstance(int index) const
{
    return index >= 0 && index < static_cast<int>(m_models.getInstances().size());
}

void Editor::validateSelection()
{
    if (!validInstance(m_gizmo.selectedInstance) && m_gizmo.selectedInstance != -1)
        m_gizmo.deselect();
    if (m_ifcSelectionInstance != -1 &&
        (!validInstance(m_ifcSelectionInstance) || !m_models.getInstances()[m_ifcSelectionInstance].ifcScene)) {
        m_ifcSelectionInstance = -1;
        m_ifcSelectionKind = IfcSelectionKind::None;
        m_selectedIfcGuid.clear();
    }
}

// Drops the current IFC element/spatial selection (flag, inspector target and outline).
void Editor::clearIfcSelection()
{
    auto& instances = m_models.getInstances();
    if (validInstance(m_ifcSelectionInstance) && instances[m_ifcSelectionInstance].ifcScene) {
        IfcScene& scene = *instances[m_ifcSelectionInstance].ifcScene;
        if (m_ifcSelectionKind == IfcSelectionKind::Element) {
            auto it = scene.elements.find(m_selectedIfcGuid);
            if (it != scene.elements.end()) it->second.selected = false;
        }
        else if (m_ifcSelectionKind == IfcSelectionKind::Spatial) {
            auto it = scene.spatial.find(m_selectedIfcGuid);
            if (it != scene.spatial.end()) it->second.selected = false;
        }
    }
    m_ifcSelectionKind = IfcSelectionKind::None;
    m_selectedIfcGuid.clear();
    m_ifcSelectionInstance = -1;
}

void Editor::selectInstance(int index)
{
    clearIfcSelection();
    m_gizmo.select(index);
}

void Editor::deselectAll()
{
    clearIfcSelection();
    m_gizmo.deselect();
}

void Editor::selectIfcElement(int instanceIndex, IfcScene& scene, const std::string& guid)
{
    clearIfcSelection();
    auto it = scene.elements.find(guid);
    if (it == scene.elements.end())
        return;
    it->second.selected = true;
    m_gizmo.select(instanceIndex);
    m_selectedIfcGuid = guid;
    m_ifcSelectionKind = IfcSelectionKind::Element;
    m_ifcSelectionInstance = instanceIndex;
}

void Editor::selectIfcSpatial(int instanceIndex, IfcScene& scene, const std::string& guid)
{
    clearIfcSelection();
    auto it = scene.spatial.find(guid);
    if (it == scene.spatial.end())
        return;
    it->second.selected = true;
    m_gizmo.select(instanceIndex);
    m_selectedIfcGuid = guid;
    m_ifcSelectionKind = IfcSelectionKind::Spatial;
    m_ifcSelectionInstance = instanceIndex;
}

void Editor::showAllIfc(IfcScene& scene)
{
    for (auto& [guid, element] : scene.elements) element.visible = true;
    for (auto& [guid, node] : scene.spatial) node.visible = true;
    scene.syncVisibilityCache();
}

void Editor::hideAllIfc(IfcScene& scene)
{
    for (auto& [guid, element] : scene.elements) element.visible = false;
    for (auto& [guid, node] : scene.spatial) node.visible = false;
    scene.syncVisibilityCache();
}

void Editor::isolateIfcElement(IfcScene& scene, const std::string& guid)
{
    for (auto& [g, element] : scene.elements) element.visible = (g == guid);
    for (auto& [g, node] : scene.spatial) node.visible = false;
    auto it = scene.elements.find(guid);
    if (it != scene.elements.end()) {
        std::string parent = it->second.parentSpatialGuid;
        while (!parent.empty()) {
            auto sit = scene.spatial.find(parent);
            if (sit == scene.spatial.end()) break;
            sit->second.visible = true;
            parent = sit->second.parentGuid;
        }
    }
    scene.syncVisibilityCache();
}

void Editor::showOnlyIfcBranch(IfcScene& scene, const std::string& spatialGuid)
{
    hideAllIfc(scene);
    scene.setVisibilityRecursive(spatialGuid, true);
    scene.syncVisibilityCache();
}

// ---------------------------------------------------------------------------------------------
// Object actions
// ---------------------------------------------------------------------------------------------

void Editor::focusOnInstance(int index)
{
    if (!validInstance(index))
        return;
    const auto& instance = m_models.getInstances()[index];
    GPUModel* model = m_models.getModel(instance.modelIndex);
    if (!model)
        return;
    const float maxScale = std::max({ instance.scale.x, instance.scale.y, instance.scale.z });
    glm::vec3 center = glm::vec3(instance.getTransformMatrix() * glm::vec4(model->boundsCenter, 1.0f));
    float radius = model->boundsRadius * maxScale;

    // Frame the selected IFC element instead of the whole model when there is one.
    if (m_ifcSelectionInstance == index && m_ifcSelectionKind == IfcSelectionKind::Element && instance.ifcScene)
        ifcElementWorldBounds(model, *instance.ifcScene, m_selectedIfcGuid, instance, center, radius);
    // 60 degree vertical FOV: a sphere of radius r fits at distance r / sin(30deg) = 2r.
    m_camera.position = center - getFront(m_camera) * std::max(radius * 2.2f, 1.0f);
}

void Editor::addModelToScene(size_t modelIndex, bool atOrigin)
{
    GPUModel* model = m_models.getModel(modelIndex);
    if (!model)
        return;
    glm::vec3 position(0.0f);
    if (!atOrigin)
        position = m_camera.position + getFront(m_camera) * std::max(model->boundsRadius * 2.2f, 2.0f) - model->boundsCenter;
    const size_t newIndex = m_models.createInstance(modelIndex, position);
    selectInstance(static_cast<int>(newIndex));
    setStatus("Added " + model->name + " to the scene");
}

void Editor::deleteInstance(int index)
{
    if (!validInstance(index))
        return;
    const std::string name = m_models.getInstances()[index].name;
    // Annotations reference instances by index.
    std::erase_if(m_annotations, [&](const Annotation& a) { return a.instanceIndex == index; });
    for (auto& annotation : m_annotations)
        if (annotation.instanceIndex > index) --annotation.instanceIndex;
    deselectAll();
    m_models.removeInstance(static_cast<size_t>(index));
    setStatus("Deleted " + name);
}

void Editor::duplicateInstance(int index)
{
    if (!validInstance(index))
        return;
    const ModelInstance source = m_models.getInstances()[index];
    GPUModel* model = m_models.getModel(source.modelIndex);
    const float offset = model ? model->boundsRadius * std::max({ source.scale.x, source.scale.y, source.scale.z }) : 1.0f;
    const size_t newIndex = m_models.createInstance(source.modelIndex, source.position + glm::vec3(offset, 0.0f, 0.0f),
        source.rotation, source.scale);
    m_models.getInstances()[newIndex].color = source.color;
    selectInstance(static_cast<int>(newIndex));
    setStatus("Duplicated " + source.name);
}

glm::vec3 Editor::instanceWorldCenter(int index) const
{
    const ModelInstance& instance = m_models.getInstances()[index];
    const auto& models = m_models.getModels();
    if (instance.modelIndex >= models.size() || !models[instance.modelIndex])
        return instance.position;
    return glm::vec3(instance.getTransformMatrix() * glm::vec4(models[instance.modelIndex]->boundsCenter, 1.0f));
}

std::string Editor::uniqueInstanceName(const std::string& base) const
{
    const auto& instances = m_models.getInstances();
    const auto taken = [&](const std::string& name) {
        return std::any_of(instances.begin(), instances.end(), [&](const ModelInstance& i) { return i.name == name; });
    };
    if (!taken(base))
        return base;
    for (int n = 1;; ++n) {
        std::string candidate = base + " (" + std::to_string(n) + ")";
        if (!taken(candidate))
            return candidate;
    }
}

void Editor::addCube()
{
    size_t modelIndex = 0;
    if (const auto found = m_models.findModelByPath(kBuiltinCubePath)) {
        modelIndex = *found;
    }
    else {
        try {
            modelIndex = m_models.loadModelSync(kBuiltinCubePath, "Cube");
        }
        catch (const std::exception& e) {
            setStatus(std::string("Failed to create cube: ") + e.what(), true);
            return;
        }
    }
    const float step = m_snapTranslate > 0.0f ? m_snapTranslate : 1.0f;
    const glm::vec3 position = glm::round((m_camera.position + getFront(m_camera) * 5.0f) / step) * step;
    const size_t newIndex = m_models.createInstance(modelIndex, position);
    const std::string name = uniqueInstanceName("Cube");
    m_models.getInstances()[newIndex].name = name;
    selectInstance(static_cast<int>(newIndex));
    setStatus("Added " + name);
}

void Editor::beginRename(int index)
{
    if (!validInstance(index))
        return;
    m_showHierarchy = true;
    m_renamingInstance = index;
    m_renameFocusPending = true;
    const std::string& name = m_models.getInstances()[index].name;
    strncpy(m_renameBuffer, name.c_str(), sizeof(m_renameBuffer) - 1);
    m_renameBuffer[sizeof(m_renameBuffer) - 1] = '\0';
}

void Editor::unloadModel(size_t modelIndex)
{
    GPUModel* model = m_models.getModel(modelIndex);
    if (!model)
        return;
    const std::string name = model->name;
    // Instances of this model disappear and the rest shift down; remap annotation indices.
    const auto& instances = m_models.getInstances();
    std::vector<int> remap(instances.size(), -1);
    int next = 0;
    for (size_t k = 0; k < instances.size(); ++k)
        remap[k] = instances[k].modelIndex == modelIndex ? -1 : next++;
    std::erase_if(m_annotations, [&](const Annotation& a) {
        return !validInstance(a.instanceIndex) || remap[a.instanceIndex] < 0;
    });
    for (auto& annotation : m_annotations)
        annotation.instanceIndex = remap[annotation.instanceIndex];
    deselectAll();
    m_models.unloadModel(modelIndex);
    setStatus("Unloaded " + name);
}

// ---------------------------------------------------------------------------------------------
// Scene actions
// ---------------------------------------------------------------------------------------------

void Editor::newScene()
{
    deselectAll();
    m_scenes.clear();
    m_annotations.clear();
    m_scenes.setCurrentPath({});
    setStatus("New scene");
}

void Editor::openScene(const std::string& path)
{
    const SceneManager::OpenResult opened = m_scenes.open(path);
    if (!opened.ok) {
        setStatus("Failed to open scene: " + path, true);
        return;
    }
    // The previous scene is gone, so selection and annotations would point at stale instances.
    deselectAll();
    m_annotations.clear();
    for (const auto& missing : opened.missingFiles)
        setStatus("Model file not found: " + missing, true);
    setStatus("Opening " + path + " (" + std::to_string(opened.queuedModels) + " models)...");
}

void Editor::saveSceneTo(const std::string& path)
{
    if (m_scenes.save(path))
        setStatus("Scene saved: " + path);
    else
        setStatus("Failed to save scene: " + path, true);
}

void Editor::saveScene()
{
    if (m_scenes.currentPath().empty())
        saveSceneAsDialog();
    else
        saveSceneTo(m_scenes.currentPath());
}

void Editor::requestAnnotation()
{
    if (m_ifcSelectionKind != IfcSelectionKind::Element || !validInstance(m_ifcSelectionInstance))
        return;
    m_annotationTargetGuid = m_selectedIfcGuid;
    m_annotationTargetInstance = m_ifcSelectionInstance;
    m_annotationText[0] = '\0';
    m_openAnnotationPopup = true;
}
