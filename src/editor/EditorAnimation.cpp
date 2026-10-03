#include "Editor.h"
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "EditorStyle.h"
#include "FileDialog.h"

namespace {

// Path polyline plus a marker per keyframe, as line-list vertex pairs.
std::vector<GizmoVertex> buildCameraPathLines(const CameraAnimator& animator)
{
    std::vector<GizmoVertex> lines;
    const auto pathPoints = animator.generatePathPoints(30);
    if (pathPoints.size() < 2)
        return lines;

    const auto& keyframes = animator.getPath().keyframes;
    lines.reserve((pathPoints.size() - 1) * 2 + keyframes.size() * 8);

    const glm::vec3 pathColor(1.0f, 1.0f, 0.0f);
    for (size_t i = 0; i < pathPoints.size() - 1; ++i) {
        lines.push_back({ pathPoints[i], pathColor });
        lines.push_back({ pathPoints[i + 1], pathColor });
    }

    // Small crosses, cyan for straight and magenta for curved segments, plus a short green
    // line along the look direction.
    const float crossSize = 0.3f;
    for (const auto& kf : keyframes) {
        const glm::vec3 color = kf.useCurve ? glm::vec3(1.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 1.0f);
        lines.push_back({ kf.position - glm::vec3(crossSize, 0, 0), color });
        lines.push_back({ kf.position + glm::vec3(crossSize, 0, 0), color });
        lines.push_back({ kf.position - glm::vec3(0, crossSize, 0), color });
        lines.push_back({ kf.position + glm::vec3(0, crossSize, 0), color });
        lines.push_back({ kf.position - glm::vec3(0, 0, crossSize), color });
        lines.push_back({ kf.position + glm::vec3(0, 0, crossSize), color });

        const glm::vec3 lookDir = glm::normalize(kf.lookTarget - kf.position);
        lines.push_back({ kf.position, glm::vec3(0.0f, 1.0f, 0.0f) });
        lines.push_back({ kf.position + lookDir * 1.0f, glm::vec3(0.0f, 1.0f, 0.0f) });
    }
    return lines;
}

bool transportButton(const char* label, bool active)
{
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, EditorStyle::kAccent);
    const bool pressed = ImGui::Button(label, ImVec2(70, 0));
    if (active) ImGui::PopStyleColor();
    return pressed;
}

} // namespace

void Editor::rebuildPathLines()
{
    m_renderer.setPathLines(buildCameraPathLines(m_cameraAnimator));
}

bool Editor::loadCameraPath()
{
    if (!m_cameraAnimator.loadPath(m_cameraPathFile))
        return false;
    strncpy(m_pathName, m_cameraAnimator.getPath().name.c_str(), sizeof(m_pathName) - 1);
    m_pathName[sizeof(m_pathName) - 1] = '\0';
    rebuildPathLines();
    setStatus(std::string("Camera path loaded: ") + m_cameraPathFile);
    return true;
}

void Editor::drawAnimationPanel()
{
    if (ImGui::Begin("Camera Animation", &m_showAnimationPanel)) {
        drawAnimationTransport();

        // Keyframe list and details side by side, file row below.
        drawKeyframeList();
        ImGui::SameLine();
        if (drawKeyframeDetails()) {
            m_cameraAnimator.getPath().recalculateDuration();
            rebuildPathLines();
        }
        drawAnimationFileRow();
        // The dialog writes the panel's file path, so it is only shown while the panel is.
        drawAnimationFileDialog();
    }
    ImGui::End();
}

void Editor::drawAnimationTransport()
{
    auto& path = m_cameraAnimator.getPath();
    const AnimationPlayState playState = m_cameraAnimator.getPlayState();
    ImGui::BeginDisabled(path.keyframes.size() < 2);
    if (transportButton("Play", playState == AnimationPlayState::Playing)) m_cameraAnimator.play();
    ImGui::SameLine(0, 2);
    if (transportButton("Pause", playState == AnimationPlayState::Paused)) m_cameraAnimator.pause();
    ImGui::SameLine(0, 2);
    if (transportButton("Stop", false)) m_cameraAnimator.stop();
    ImGui::EndDisabled();
    ImGui::SameLine(0, 12);
    ImGui::Checkbox("Loop", &path.loop);
    ImGui::SameLine(0, 12);
    float speed = m_cameraAnimator.getPlaybackSpeed();
    ImGui::SetNextItemWidth(140);
    if (ImGui::SliderFloat("##speed", &speed, 0.1f, 5.0f, "Speed %.1fx"))
        m_cameraAnimator.setPlaybackSpeed(speed);
    ImGui::SameLine(0, 12);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##pathName", "Path name", m_pathName, sizeof(m_pathName)))
        path.name = m_pathName;

    const float duration = m_cameraAnimator.getDuration();
    const float current = m_cameraAnimator.getCurrentTime();
    char progressText[64];
    snprintf(progressText, sizeof(progressText), "%.1fs / %.1fs", current, duration);
    ImGui::ProgressBar(duration > 0.0f ? current / duration : 0.0f, ImVec2(-FLT_MIN, 0), progressText);
}

namespace {
float animationFooterHeight()
{
    return ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
}
}

void Editor::drawKeyframeList()
{
    const auto& keyframes = m_cameraAnimator.getPath().keyframes;
    const float listWidth = ImGui::GetContentRegionAvail().x * 0.55f;
    if (ImGui::BeginChild("##keyframeList", ImVec2(listWidth, -animationFooterHeight()), ImGuiChildFlags_Borders)) {
        ImGui::SetNextItemWidth(90);
        ImGui::DragFloat("##newTime", &m_newKeyframeTime, 0.1f, 0.0f, 600.0f, "at %.1f s");
        ImGui::SameLine();
        ImGui::Checkbox("Curve", &m_newKeyframeCurved);
        ImGui::SameLine();
        if (ImGui::Button("+ Keyframe from Camera")) {
            m_cameraAnimator.addKeyframe(CameraAnimator::makeKeyframe(
                m_camera.position, m_camera.yaw, m_camera.pitch, m_newKeyframeTime, m_newKeyframeCurved));
            m_newKeyframeTime += 2.0f;
            rebuildPathLines();
        }

        if (keyframes.empty()) {
            ImGui::TextDisabled("No keyframes. Position the camera and click '+ Keyframe from Camera'.");
        }
        else if (ImGui::BeginTable("##keyframes", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0f);
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Interpolation", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (int k = 0; k < static_cast<int>(keyframes.size()); ++k) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char label[16];
                snprintf(label, sizeof(label), "%d", k);
                if (ImGui::Selectable(label, m_selectedKeyframe == k, ImGuiSelectableFlags_SpanAllColumns))
                    m_selectedKeyframe = k;
                ImGui::TableNextColumn();
                ImGui::Text("%.2f s", keyframes[k].timestamp);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", keyframes[k].useCurve ? "Curve" : "Linear");
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

bool Editor::drawKeyframeDetails()
{
    auto& keyframes = m_cameraAnimator.getPath().keyframes;
    bool modified = false;
    if (ImGui::BeginChild("##keyframeDetails", ImVec2(0, -animationFooterHeight()), ImGuiChildFlags_Borders)) {
        if (m_selectedKeyframe >= static_cast<int>(keyframes.size())) m_selectedKeyframe = -1;
        if (m_selectedKeyframe < 0) {
            ImGui::TextDisabled("Select a keyframe to edit it.");
        }
        else {
            auto& kf = keyframes[m_selectedKeyframe];
            ImGui::Text("Keyframe %d", m_selectedKeyframe);
            ImGui::Separator();
            modified |= EditorStyle::vec3Control("Position", &kf.position.x, 0.0f, 0.1f, 0.0f, 0.0f, 80.0f);
            modified |= EditorStyle::vec3Control("Look at", &kf.lookTarget.x, 0.0f, 0.1f, 0.0f, 0.0f, 80.0f);
            EditorStyle::propertyLabel("Time", 80.0f);
            modified |= ImGui::DragFloat("##time", &kf.timestamp, 0.1f, 0.0f, 600.0f, "%.2f s");
            EditorStyle::propertyLabel("Curve", 80.0f);
            modified |= ImGui::Checkbox("##curve", &kf.useCurve);

            if (ImGui::Button("Go to")) {
                m_camera.position = kf.position;
                const glm::vec3 dir = glm::normalize(kf.lookTarget - kf.position);
                m_camera.yaw = glm::degrees(std::atan2(dir.z, dir.x));
                m_camera.pitch = glm::degrees(std::asin(glm::clamp(dir.y, -1.0f, 1.0f)));
            }
            ImGui::SetItemTooltip("Move the camera to this keyframe");
            ImGui::SameLine();
            if (ImGui::Button("Set from Camera")) {
                kf = CameraAnimator::makeKeyframe(m_camera.position, m_camera.yaw, m_camera.pitch, kf.timestamp, kf.useCurve);
                modified = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                m_cameraAnimator.removeKeyframe(static_cast<size_t>(m_selectedKeyframe));
                m_selectedKeyframe = -1;
                modified = true;
            }
        }
    }
    ImGui::EndChild();
    return modified;
}

void Editor::drawAnimationFileRow()
{
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
    ImGui::InputTextWithHint("##animFile", "camera_path.cmap", m_cameraPathFile, sizeof(m_cameraPathFile));
    ImGui::SameLine();
    if (ImGui::Button("Open..."))
        openFileDialog("BrowseAnimDlg", "Open Camera Path", ".cmap", kCameraPathsRoot, nullptr, false);
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        m_cameraAnimator.savePath(m_cameraPathFile);
        setStatus(std::string("Camera path saved: ") + m_cameraPathFile);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload") && !loadCameraPath())
        setStatus(std::string("Failed to load camera path: ") + m_cameraPathFile, true);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_cameraAnimator.getPath().keyframes.empty());
    if (ImGui::Button("Clear Keyframes")) {
        m_cameraAnimator.clearKeyframes();
        m_selectedKeyframe = -1;
        rebuildPathLines();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool canExport = VideoEncoder::supported() && m_cameraAnimator.getPath().keyframes.size() >= 2 &&
        m_cameraAnimator.getDuration() > 0.0f;
    ImGui::BeginDisabled(!canExport);
    if (ImGui::Button("Export Video...")) m_openVideoExportPopup = true;
    ImGui::EndDisabled();
    ImGui::SetItemTooltip(VideoEncoder::supported()
        ? "Render the camera path to an .mp4 video (needs at least two keyframes)"
        : "Video export is only available on Windows");
}

void Editor::drawAnimationFileDialog()
{
    ImGuiFileDialog* dialog = ImGuiFileDialog::Instance();
    if (!dialog->Display("BrowseAnimDlg", ImGuiWindowFlags_NoCollapse, kDialogSize))
        return;
    if (dialog->IsOk()) {
        const auto rel = makeRelativeIfInside(dialog->GetFilePathName(), kCameraPathsRoot);
        if (rel.has_value()) {
            strncpy(m_cameraPathFile, rel.value().c_str(), sizeof(m_cameraPathFile) - 1);
            m_cameraPathFile[sizeof(m_cameraPathFile) - 1] = '\0';
            loadCameraPath();
        }
        else {
            setStatus("Camera paths must be inside the application folder", true);
        }
    }
    dialog->Close();
}
