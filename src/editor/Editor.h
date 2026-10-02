#pragma once
#include <array>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <glm/glm.hpp>
#include "imgui.h"
#include "app/AppMode.h"
#include "engine/Camera.h"
#include "engine/CameraAnimation.h"
#include "engine/Gizmo.h"
#include "engine/GraphicsSettings.h"
#include "engine/IfcScene.h"
#include "engine/ModelManager.h"
#include "engine/Renderer.h"

// The scene editor: viewport interaction (picking, gizmo, camera) plus the docked ImGui panels.
// The implementation is split by panel across the Editor*.cpp files.
class Editor final : public AppMode {
public:
    explicit Editor(const EngineContext& engine);

    Editor(const Editor&) = delete;
    Editor& operator=(const Editor&) = delete;

    void onEvent(const SDL_Event& event) override;
    void update(float dt) override;
    void lateUpdate(float dt) override;
    bool uiVisible() const override { return !m_flyMode; }
    void drawUi() override;
    void fillFrame(FrameInput& frame) override;
    bool quitRequested() const override { return m_quitRequested; }

    // Replaces the current scene and reports the outcome in the status bar.
    void openScene(const std::string& path);

private:
    enum class IfcSelectionKind {
        None,
        Element,
        Spatial,
    };

    // Flat search results for one IFC instance, rebuilt when the filter or element count changes.
    struct SearchCache {
        std::string filter;
        size_t elementCount = 0;
        std::vector<std::string> guids;
    };

    // Hierarchy actions are applied after the tree is drawn so the instance list is stable while drawing.
    struct HierarchyActions {
        int deleteIndex = -1;
        int duplicateIndex = -1;
        int focusIndex = -1;
        bool createCube = false;
    };

    // "Elements with this name" counts walk every element, so they are cached per selection.
    struct SameNameCount {
        std::string guid;
        int instance = -1;
        size_t inModel = 0;
        size_t inScene = 0;
    };

    static constexpr const char* kScenesRoot = ".";
    static constexpr const char* kModelsRoot = "models";
    static constexpr const char* kCameraPathsRoot = ".";
    static constexpr ImVec2 kDialogSize{ 760.0f, 480.0f };
    static constexpr float kGizmoPickRadius = 12.0f;

    // One end of the scene orientation gizmo, in scene-view coordinates.
    struct OrientationHandle {
        int axis = 0; // 0..2 = +X +Y +Z, 3..5 = -X -Y -Z
        glm::vec3 direction{ 0.0f };
        glm::vec2 position{ 0.0f };
        float depth = 0.0f; // larger = closer to the viewer
    };

    // Smooth camera turn started from the orientation gizmo.
    struct ViewTurn {
        bool active = false;
        float elapsed = 0.0f;
        glm::vec3 pivot{ 0.0f };
        glm::vec3 startPosition{ 0.0f };
        float startYaw = 0.0f;
        float startPitch = 0.0f;
        float targetYaw = 0.0f;
        float targetPitch = 0.0f;
        float distance = 10.0f;
    };

    // ---- Editor.cpp: input ----
    void handleKeyDown(const SDL_KeyboardEvent& key);
    void dropStaleGizmoSelection();
    void handleViewportMouse(const SDL_Event& event);
    void handleViewportClick(float mouseX, float mouseY);
    bool tryBeginGizmoDrag(float mouseX, float mouseY, const glm::mat4& view, const glm::mat4& proj);
    void selectPickedSubmesh(const SubmeshHitResult& hit);
    void dragGizmo(float mouseX, float mouseY);
    bool gizmoVisible() const;
    GizmoShape currentGizmoShape() const;
    void handleCameraLook(const SDL_Event& event);
    void moveCamera(float dt);
    void setFlyMode(bool enabled);
    bool sceneViewContains(float x, float y) const;
    // Projection of the scene view; rendering, picking, gizmos and annotations all use this one.
    glm::mat4 sceneProjection() const;

    // ---- Editor.cpp: selection and actions ----
    bool validInstance(int index) const;
    bool hasSelection() const { return validInstance(m_gizmo.selectedInstance); }
    void validateSelection();
    void clearIfcSelection();
    void selectInstance(int index);
    void deselectAll();
    void selectIfcElement(int instanceIndex, IfcScene& scene, const std::string& guid);
    void selectIfcSpatial(int instanceIndex, IfcScene& scene, const std::string& guid);
    static void showAllIfc(IfcScene& scene);
    static void hideAllIfc(IfcScene& scene);
    static void isolateIfcElement(IfcScene& scene, const std::string& guid);
    static void showOnlyIfcBranch(IfcScene& scene, const std::string& spatialGuid);
    void focusOnInstance(int index);
    void addModelToScene(size_t modelIndex, bool atOrigin);
    void deleteInstance(int index);
    void duplicateInstance(int index);
    void unloadModel(size_t modelIndex);
    void newScene();
    void saveSceneTo(const std::string& path);
    void saveScene();
    void requestAnnotation();
    void handleShortcuts();
    void updateWindowTitle();
    void setStatus(const std::string& message, bool isError = false);
    static std::string formatCount(size_t value);
    void fillHighlight(SelectionHighlight& highlight) const;
    static void collectSpatialSubmeshes(const IfcScene& scene, const std::string& spatialGuid, std::vector<uint32_t>& out);
    // World-space bounding sphere of an IFC element over all of its submeshes. False if it has no bounds.
    static bool ifcElementWorldBounds(const GPUModel* model, const IfcScene& scene, const std::string& guid,
        const ModelInstance& instance, glm::vec3& center, float& radius);
    glm::vec3 instanceWorldCenter(int index) const;
    void addCube();
    std::string uniqueInstanceName(const std::string& base) const;
    void beginRename(int index);

    // ---- EditorMenus.cpp ----
    void drawMainMenuBar();
    void drawFileMenu();
    void drawEditMenu();
    void drawViewMenu();
    void drawToolbar();
    void drawToolButton(const char* label, GizmoMode tool, const char* tooltip);
    void drawSnapPopup();
    void drawAddMenu();
    void drawStatusBar();
    void drawDockSpace();
    void buildDefaultLayout(ImGuiID dockspaceId);
    void drawViewportOverlay();
    void drawHelpPopups();
    void drawGraphicsSettingsWindow();
    void drawControlsPopup();
    void drawAboutPopup();
    void openFileDialog(const char* key, const char* title, const char* filters, const std::string& root,
        const char* defaultFileName, bool confirmOverwrite);
    void openSceneDialog();
    void saveSceneAsDialog();
    void importModelDialog();
    void drawFileDialogs();

    // ---- EditorGizmo.cpp ----
    void drawTransformGizmo();
    std::array<OrientationHandle, 6> orientationHandles() const;
    glm::vec2 orientationCenter() const;
    int pickOrientationHandle(float x, float y) const;
    void drawOrientationGizmo();
    void beginViewTurn(int axis);
    void updateViewTurn(float dt);

    // ---- EditorHierarchy.cpp ----
    void drawHierarchy();
    void drawModelList();
    void drawSceneTree();
    int expandTreeToViewportSelection();
    const std::vector<std::string>* updateSearchCache(int instanceIndex);
    void drawInstanceNode(int instanceIndex, int forceOpenInstance, HierarchyActions& actions);
    void drawInstanceContextMenu(int instanceIndex, HierarchyActions& actions);
    bool drawRenameField(int instanceIndex);
    void drawElementRow(int instanceIndex, IfcScene& scene, IfcElement& element);
    void drawElementContextMenu(int instanceIndex, IfcScene& scene, IfcElement& element);
    void drawSpatialNode(int instanceIndex, IfcScene& scene, const std::string& spatialGuid);
    void drawSpatialContextMenu(int instanceIndex, IfcScene& scene, IfcSpatialNode& node);
    void drawSpatialElements(int instanceIndex, IfcScene& scene, IfcSpatialNode& node);

    // ---- EditorInspector.cpp ----
    void drawInspector();
    void drawInspectorHeader(ModelInstance& instance, const GPUModel* model);
    void drawTransformSection(int instanceIndex);
    void drawIfcSelectionDetails();
    const std::unordered_map<std::string, std::string>* drawIfcElementSection(IfcScene& scene);
    const std::unordered_map<std::string, std::string>* drawIfcSpatialSection(IfcScene& scene);
    void drawSameNameCounts(const IfcElement& element);
    void drawIfcProperties(const std::unordered_map<std::string, std::string>& properties);

    // ---- EditorAnnotations.cpp ----
    void drawAnnotationPopup();
    void addAnnotationFromPopup();
    void drawViewportAnnotations();
    void drawAnnotationsPanel();
    void drawAnnotationTable();

    // ---- EditorAnimation.cpp ----
    void drawAnimationPanel();
    void drawAnimationTransport();
    void drawKeyframeList();
    bool drawKeyframeDetails();
    void drawAnimationFileRow();
    void drawAnimationFileDialog();
    bool loadCameraPath();
    void rebuildPathLines();

    // ---- EditorStatistics.cpp ----
    void drawStatisticsPanel();

    SDL_Window* m_window;
    const VulkanContext& m_vulkan;
    Renderer& m_renderer;
    ModelManager& m_models;
    SceneManager& m_scenes;
    GraphicsSettings& m_settings;

    // Camera and viewport
    Camera m_camera;
    bool m_flyMode = false; // UI hidden, mouse always looks around
    bool m_rightMouseHeld = false;
    float m_cameraSpeedMultiplier = 1.0f;
    // Scene viewport = central dock area in window coordinates. The 3D scene renders only here,
    // and mouse picking / gizmo / annotation math is relative to it.
    ViewRect m_sceneView;
    CameraAnimator m_cameraAnimator;
    bool m_showCameraPath = true;
    bool m_showGrid = true;

    // Selection and tools
    Gizmo m_gizmo;
    GizmoMode m_tool = GizmoMode::Translate;
    IfcSelectionKind m_ifcSelectionKind = IfcSelectionKind::None;
    std::string m_selectedIfcGuid;
    int m_ifcSelectionInstance = -1;
    bool m_selectionChangedFromViewport = false;
    GizmoAxis m_hoveredAxis = GizmoAxis::None;
    // Ctrl while dragging: absolute grid step for moves, increments for rotate/scale deltas.
    float m_snapTranslate = 1.0f;
    float m_snapRotate = 15.0f;
    float m_snapScale = 0.1f;

    // Orientation gizmo
    int m_hoveredOrientation = -1;
    ViewTurn m_viewTurn;

    // Annotations
    std::vector<Annotation> m_annotations;
    bool m_showAnnotations = true;
    bool m_openAnnotationPopup = false;
    char m_annotationText[256] = "";
    std::string m_annotationTargetGuid;
    int m_annotationTargetInstance = -1;

    // Panels and popups
    bool m_showHierarchy = true;
    bool m_showInspector = true;
    bool m_showAnimationPanel = true;
    bool m_showAnnotationsPanel = true;
    bool m_showStatisticsPanel = true;
    bool m_showGraphicsSettings = false;
    bool m_resetLayout = false;
    bool m_openControlsPopup = false;
    bool m_openAboutPopup = false;
    bool m_quitRequested = false;
    std::string m_windowTitle;

    // Status bar
    std::string m_statusMessage = "Ready";
    bool m_statusIsError = false;
    double m_statusTime = 0.0;

    // Hierarchy
    ImGuiTextFilter m_hierarchyFilter;
    std::vector<SearchCache> m_searchCaches;
    // Spatial nodes on the path to a viewport-picked element, force-opened for a few frames.
    std::unordered_set<std::string> m_openSpatialGuids;
    int m_openPathFramesLeft = 0;
    int m_renamingInstance = -1;
    bool m_renameFocusPending = false;
    char m_renameBuffer[256] = "";

    // Inspector
    ImGuiTextFilter m_propertyFilter;
    SameNameCount m_sameNameCount;

    // Camera animation panel
    char m_pathName[128] = "CameraPath1";
    char m_cameraPathFile[256] = "camera_path.cmap";
    float m_newKeyframeTime = 0.0f;
    bool m_newKeyframeCurved = true;
    int m_selectedKeyframe = -1;

    // Statistics panel
    float m_frameTimes[240] = {};
    int m_frameTimeOffset = 0;
};
