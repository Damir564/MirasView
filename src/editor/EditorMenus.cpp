#include "Editor.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include "imgui_internal.h"
#include "EditorStyle.h"
#include "FileDialog.h"
#include "app/SettingsUi.h"
#include "engine/ModelManager.h"
#include "engine/SceneManager.h"

// ---------------------------------------------------------------------------------------------
// Menu bar
// ---------------------------------------------------------------------------------------------

void Editor::drawMainMenuBar()
{
    if (!ImGui::BeginMainMenuBar())
        return;
    drawFileMenu();
    drawEditMenu();
    drawAddMenu();
    drawViewMenu();
    if (ImGui::BeginMenu("Settings")) {
        ImGui::MenuItem("Graphics...", nullptr, &m_showGraphicsSettings);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("Controls", "F1")) m_openControlsPopup = true;
        if (ImGui::MenuItem("About MirasEngine")) m_openAboutPopup = true;
        ImGui::EndMenu();
    }

    const std::string& path = m_scenes.currentPath();
    const char* sceneLabel = path.empty() ? "Untitled scene" : path.c_str();
    const float labelWidth = ImGui::CalcTextSize(sceneLabel).x;
    ImGui::SameLine(ImGui::GetWindowWidth() - labelWidth - ImGui::GetStyle().WindowPadding.x * 2);
    ImGui::TextDisabled("%s", sceneLabel);
    ImGui::EndMainMenuBar();
}

void Editor::drawFileMenu()
{
    if (!ImGui::BeginMenu("File"))
        return;
    if (ImGui::MenuItem("New Scene")) newScene();
    if (ImGui::MenuItem("Open Scene...", "Ctrl+O")) openSceneDialog();
    if (ImGui::MenuItem("Save Scene", "Ctrl+S")) saveScene();
    if (ImGui::MenuItem("Save Scene As...", "Ctrl+Shift+S")) saveSceneAsDialog();
    ImGui::Separator();
    if (ImGui::MenuItem("Import Model...", "Ctrl+I")) importModelDialog();
    ImGui::Separator();
    if (ImGui::MenuItem("Exit", "Alt+F4")) m_quitRequested = true;
    ImGui::EndMenu();
}

void Editor::drawEditMenu()
{
    if (!ImGui::BeginMenu("Edit"))
        return;
    const bool selection = hasSelection();
    if (ImGui::MenuItem("Rename", "F2", false, selection)) beginRename(m_gizmo.selectedInstance);
    if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, selection)) duplicateInstance(m_gizmo.selectedInstance);
    if (ImGui::MenuItem("Delete", "Del", false, selection)) deleteInstance(m_gizmo.selectedInstance);
    if (ImGui::MenuItem("Deselect", "Esc", false, selection)) deselectAll();
    ImGui::Separator();
    if (ImGui::MenuItem("Focus Selected", "F", false, selection)) focusOnInstance(m_gizmo.selectedInstance);
    if (ImGui::MenuItem("Add Annotation...", "M", false, m_ifcSelectionKind == IfcSelectionKind::Element)) requestAnnotation();
    ImGui::Separator();
    if (ImGui::MenuItem("Select Tool", "Q", m_tool == GizmoMode::None)) m_tool = GizmoMode::None;
    if (ImGui::MenuItem("Move Tool", "1", m_tool == GizmoMode::Translate)) m_tool = GizmoMode::Translate;
    if (ImGui::MenuItem("Rotate Tool", "2", m_tool == GizmoMode::Rotate)) m_tool = GizmoMode::Rotate;
    if (ImGui::MenuItem("Scale Tool", "3", m_tool == GizmoMode::Scale)) m_tool = GizmoMode::Scale;
    ImGui::EndMenu();
}

void Editor::drawAddMenu()
{
    if (!ImGui::BeginMenu("Add"))
        return;
    if (ImGui::MenuItem("Cube")) addCube();
    ImGui::EndMenu();
}

void Editor::drawViewMenu()
{
    if (!ImGui::BeginMenu("View"))
        return;
    ImGui::MenuItem("Hierarchy", nullptr, &m_showHierarchy);
    ImGui::MenuItem("Inspector", nullptr, &m_showInspector);
    ImGui::MenuItem("Camera Animation", nullptr, &m_showAnimationPanel);
    ImGui::MenuItem("Annotations", nullptr, &m_showAnnotationsPanel);
    ImGui::MenuItem("Statistics", nullptr, &m_showStatisticsPanel);
    ImGui::Separator();
    ImGui::MenuItem("Show Annotations in Viewport", nullptr, &m_showAnnotations);
    ImGui::MenuItem("Show Camera Path", nullptr, &m_showCameraPath);
    ImGui::MenuItem("Grid", nullptr, &m_showGrid);
    ImGui::Separator();
    if (ImGui::MenuItem("Fly Mode (hide UI)", "Shift+`")) setFlyMode(true);
    if (ImGui::MenuItem("Reset Layout")) m_resetLayout = true;
    ImGui::EndMenu();
}

// ---------------------------------------------------------------------------------------------
// Toolbar and status bar
// ---------------------------------------------------------------------------------------------

namespace {
constexpr ImGuiWindowFlags kBarFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
}

void Editor::drawToolButton(const char* label, GizmoMode tool, const char* tooltip)
{
    const bool active = m_tool == tool;
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, EditorStyle::kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorStyle::kAccentHover);
    }
    if (ImGui::Button(label, ImVec2(64, 0))) m_tool = tool;
    if (active) ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("%s", tooltip);
    ImGui::SameLine(0, 2);
}

void Editor::drawToolbar()
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 5));
    if (ImGui::BeginViewportSideBar("##Toolbar", ImGui::GetMainViewport(), ImGuiDir_Up, ImGui::GetFrameHeight() + 10.0f, kBarFlags)) {
        drawToolButton("Select", GizmoMode::None, "Select objects without a transform gizmo (Q)");
        drawToolButton("Move", GizmoMode::Translate, "Move the selected object (1)");
        drawToolButton("Rotate", GizmoMode::Rotate, "Rotate the selected object (2)");
        drawToolButton("Scale", GizmoMode::Scale, "Scale the selected object (3)");

        ImGui::SameLine(0, 16);
        ImGui::BeginDisabled(!hasSelection());
        if (ImGui::Button("Focus")) focusOnInstance(m_gizmo.selectedInstance);
        ImGui::SetItemTooltip("Move the camera to the selection (F)");
        ImGui::EndDisabled();

        ImGui::SameLine(0, 16);
        if (ImGui::Button("Snap")) ImGui::OpenPopup("SnapSettings");
        ImGui::SetItemTooltip("Snap increments used while holding Ctrl during a gizmo drag");
        drawSnapPopup();

        ImGui::SameLine(0, 16);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Camera speed");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::SliderFloat("##cameraSpeed", &m_camera.speed, 0.5f, 200.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
        ImGui::SetItemTooltip("WASD movement speed (hold Shift for 4x)");

        ImGui::SameLine(0, 16);
        ImGui::Checkbox("Grid", &m_showGrid);
        ImGui::SetItemTooltip("Show the ground grid (View > Grid)");

        ImGui::SameLine(0, 16);
        ImGui::Checkbox("Annotations", &m_showAnnotations);
        ImGui::SameLine();
        ImGui::Checkbox("Camera path", &m_showCameraPath);

        const float flyWidth = ImGui::CalcTextSize("Fly Mode").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetWindowWidth() - flyWidth - 8);
        if (ImGui::Button("Fly Mode")) setFlyMode(true);
        ImGui::SetItemTooltip("Hide the UI and look around with the mouse (Shift+` or Esc to exit)");
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

void Editor::drawSnapPopup()
{
    if (!ImGui::BeginPopup("SnapSettings"))
        return;
    ImGui::TextDisabled("Hold Ctrl while dragging the gizmo");
    ImGui::PushItemWidth(120);
    if (ImGui::DragFloat("Move (grid step)", &m_snapTranslate, 0.05f, 0.01f, 100.0f, "%.2f"))
        m_snapTranslate = std::max(m_snapTranslate, 0.01f);
    if (ImGui::DragFloat("Rotate (degrees)", &m_snapRotate, 0.5f, 0.1f, 180.0f, "%.1f"))
        m_snapRotate = std::max(m_snapRotate, 0.1f);
    if (ImGui::DragFloat("Scale", &m_snapScale, 0.01f, 0.01f, 10.0f, "%.2f"))
        m_snapScale = std::max(m_snapScale, 0.01f);
    ImGui::PopItemWidth();
    if (ImGui::Button("Reset")) {
        m_snapTranslate = 1.0f;
        m_snapRotate = 15.0f;
        m_snapScale = 0.1f;
    }
    ImGui::EndPopup();
}

void Editor::drawStatusBar()
{
    if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, ImGui::GetFrameHeight(),
            kBarFlags | ImGuiWindowFlags_MenuBar) &&
        ImGui::BeginMenuBar()) {
        const auto& tasks = m_models.getLoadingTasks();
        if (!tasks.empty()) {
            static constexpr char kSpinner[] = "|/-\\";
            const char spin = kSpinner[static_cast<int>(ImGui::GetTime() * 8.0) % 4];
            const std::string more = tasks.size() > 1 ? " (+" + std::to_string(tasks.size() - 1) + " more)" : "";
            ImGui::TextColored(EditorStyle::kHighlight, "%c  Loading %s%s", spin, tasks.front().name.c_str(), more.c_str());
        }
        else {
            const bool recent = ImGui::GetTime() - m_statusTime < 5.0;
            const ImVec4 color = m_statusIsError ? EditorStyle::kError
                : (recent ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : EditorStyle::kTextDim);
            ImGui::TextColored(color, "%s", m_statusMessage.c_str());
        }

        const auto& instances = m_models.getInstances();
        size_t triangles = 0;
        for (const auto& instance : instances)
            if (instance.visible)
                if (GPUModel* model = m_models.getModel(instance.modelIndex)) triangles += model->indexCount / 3;
        const ImGuiIO& io = ImGui::GetIO();
        char right[256];
        snprintf(right, sizeof(right), "Objects: %zu   Models: %zu   Triangles: %s   |   %.0f FPS (%.2f ms)",
            instances.size(), m_models.getModels().size(), formatCount(triangles).c_str(),
            io.Framerate, 1000.0f / std::max(io.Framerate, 0.001f));
        const float rightWidth = ImGui::CalcTextSize(right).x;
        ImGui::SameLine(ImGui::GetWindowWidth() - rightWidth - 12);
        ImGui::TextDisabled("%s", right);
        ImGui::EndMenuBar();
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Dock space and viewport
// ---------------------------------------------------------------------------------------------

void Editor::buildDefaultLayout(ImGuiID dockspaceId)
{
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->WorkSize);
    ImGuiID center = dockspaceId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Camera Animation", bottom);
    ImGui::DockBuilderDockWindow("Annotations", bottom);
    ImGui::DockBuilderDockWindow("Statistics", bottom);
    ImGui::DockBuilderFinish(dockspaceId);
    m_showHierarchy = m_showInspector = m_showAnimationPanel = m_showAnnotationsPanel = m_showStatisticsPanel = true;
}

void Editor::drawDockSpace()
{
    ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    const ImGuiID dockspaceId = ImGui::GetID("EditorDockSpace");
    if (m_resetLayout || ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
        m_resetLayout = false;
        buildDefaultLayout(dockspaceId);
    }
    ImGui::DockSpaceOverViewport(dockspaceId, mainViewport, ImGuiDockNodeFlags_PassthruCentralNode);

    // The 3D scene is rendered into the central (empty) dock area.
    if (ImGuiDockNode* centralNode = ImGui::DockBuilderGetCentralNode(dockspaceId)) {
        m_sceneView = { centralNode->Pos.x, centralNode->Pos.y,
            std::max(centralNode->Size.x, 1.0f), std::max(centralNode->Size.y, 1.0f) };
    }
    else {
        m_sceneView = { mainViewport->WorkPos.x, mainViewport->WorkPos.y,
            std::max(mainViewport->WorkSize.x, 1.0f), std::max(mainViewport->WorkSize.y, 1.0f) };
    }
}

void Editor::drawViewportOverlay()
{
    ImGui::SetNextWindowPos(ImVec2(m_sceneView.x + 10, m_sceneView.y + 10));
    ImGui::SetNextWindowBgAlpha(0.55f);
    const ImGuiWindowFlags overlayFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoMouseInputs;
    if (ImGui::Begin("##ViewportOverlay", nullptr, overlayFlags)) {
        const char* toolName = m_tool == GizmoMode::None ? "Select"
            : m_tool == GizmoMode::Translate ? "Move"
            : m_tool == GizmoMode::Rotate ? "Rotate" : "Scale";
        ImGui::Text("Perspective  |  %s tool", toolName);
        ImGui::TextDisabled("LMB select   RMB+drag look   WASD move   F focus");
        ImGui::TextDisabled("Ctrl+drag snap: %.2f / %.1f deg / %.2f", m_snapTranslate, m_snapRotate, m_snapScale);
    }
    ImGui::End();
}

// ---------------------------------------------------------------------------------------------
// Help popups
// ---------------------------------------------------------------------------------------------

void Editor::drawHelpPopups()
{
    if (m_openControlsPopup) ImGui::OpenPopup("Controls");
    if (m_openAboutPopup) ImGui::OpenPopup("About MirasEngine");
    m_openControlsPopup = false;
    m_openAboutPopup = false;
    drawControlsPopup();
    drawAboutPopup();
}

void Editor::drawControlsPopup()
{
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (ImGui::BeginTable("##controls", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthFixed, 170.0f);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 300.0f);
        static constexpr const char* kRows[][2] = {
            { "Left click", "Select object / IFC element" },
            { "Right mouse + drag", "Look around" },
            { "W A S D", "Move camera" },
            { "Shift (hold)", "Move 4x faster" },
            { "F", "Focus selection" },
            { "Q / 1 / 2 / 3", "Select / Move / Rotate / Scale tool" },
            { "Ctrl (while dragging)", "Snap move to grid / rotate and scale to steps" },
            { "Click view gizmo axis", "Look along that axis (top-right of viewport)" },
            { "F2", "Rename selected object" },
            { "M", "Annotate selected IFC element" },
            { "Ctrl+D", "Duplicate selected object" },
            { "Delete", "Delete selected object" },
            { "Esc", "Deselect / leave fly mode" },
            { "Shift+`", "Toggle fly mode (hide UI)" },
            { "Ctrl+O / Ctrl+S", "Open / save scene" },
            { "Ctrl+Shift+S", "Save scene as" },
            { "Ctrl+I", "Import model" },
        };
        for (const auto& row : kRows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(EditorStyle::kHighlight, "%s", row[0]);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row[1]);
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Editor::drawAboutPopup()
{
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("About MirasEngine", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::Text("MirasEngine");
    ImGui::TextDisabled("Vulkan 1.3 renderer and IFC/BIM viewer");
    ImGui::Separator();
    ImGui::Text("Dear ImGui %s", ImGui::GetVersion());
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---------------------------------------------------------------------------------------------
// File dialogs
// ---------------------------------------------------------------------------------------------

void Editor::openFileDialog(const char* key, const char* title, const char* filters, const std::string& root,
    const char* defaultFileName, bool confirmOverwrite)
{
    namespace fs = std::filesystem;
    fs::create_directories(root);
    IGFD::FileDialogConfig config;
    config.path = fs::weakly_canonical(fs::absolute(root)).string();
    config.countSelectionMax = 1;
    config.flags = ImGuiFileDialogFlags_Modal;
    if (confirmOverwrite) config.flags |= ImGuiFileDialogFlags_ConfirmOverwrite;
    if (defaultFileName) config.fileName = defaultFileName;
    ImGuiFileDialog::Instance()->OpenDialog(key, title, filters, config);
}

void Editor::openSceneDialog()
{
    openFileDialog("BrowseSceneDlg", "Open Scene", ".scn", kScenesRoot, nullptr, false);
}

void Editor::saveSceneAsDialog()
{
    openFileDialog("SaveAsSceneDlg", "Save Scene As", ".scn", kScenesRoot, "untitled.scn", true);
}

void Editor::importModelDialog()
{
    openFileDialog("BrowseModelDlg", "Import 3D Model",
        "3D Models{.gltf,.glb,.ifc},.gltf,.glb,.ifc", kModelsRoot, nullptr, false);
}

void Editor::drawFileDialogs()
{
    ImGuiFileDialog* dialog = ImGuiFileDialog::Instance();
    if (dialog->Display("BrowseSceneDlg", ImGuiWindowFlags_NoCollapse, kDialogSize)) {
        if (dialog->IsOk()) {
            openScene(toStoredPath(dialog->GetFilePathName()));
        }
        dialog->Close();
    }
    if (dialog->Display("SaveAsSceneDlg", ImGuiWindowFlags_NoCollapse, kDialogSize)) {
        if (dialog->IsOk()) {
            std::filesystem::path path(dialog->GetFilePathName());
            if (!path.has_extension() || path.extension() != ".scn") path.replace_extension(".scn");
            m_scenes.setCurrentPath(toStoredPath(path.string()));
            saveSceneTo(m_scenes.currentPath());
        }
        dialog->Close();
    }
    if (dialog->Display("BrowseModelDlg", ImGuiWindowFlags_NoCollapse, kDialogSize)) {
        if (dialog->IsOk()) {
            const std::string path = toStoredPath(dialog->GetFilePathName());
            const std::string name = std::filesystem::path(path).stem().string();
            m_models.loadModelAsync(path, name);
            setStatus("Importing " + name + "...");
        }
        dialog->Close();
    }
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

void Editor::drawGraphicsSettingsWindow()
{
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Graphics Settings", &m_showGraphicsSettings))
        drawGraphicsSettings(m_settings, m_renderer.capabilities());
    ImGui::End();
}
