#include "Editor.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include "EditorStyle.h"
#include "FileDialog.h"

namespace {

struct ResolutionPreset {
    const char* label;
    int width;
    int height;
};

// The last entry is the custom size.
constexpr ResolutionPreset kResolutions[] = {
    { "1280 x 720 (720p)", 1280, 720 },
    { "1920 x 1080 (1080p)", 1920, 1080 },
    { "2560 x 1440 (1440p)", 2560, 1440 },
    { "3840 x 2160 (4K)", 3840, 2160 },
    { "Custom", 0, 0 },
};
constexpr int kCustomResolution = IM_ARRAYSIZE(kResolutions) - 1;
constexpr int kFrameRates[] = { 24, 25, 30, 50, 60 };
// Rendering stops for this frame once it took this long, so the progress popup stays responsive.
constexpr uint64_t kExportBudgetNs = 30'000'000;

int evenAtLeast(int value, int minimum)
{
    return std::max(value, minimum) & ~1;
}

int exportFrameCount(float duration, float speed, int fps)
{
    // Both ends included: the first frame shows the first keyframe and the last frame the last one.
    return static_cast<int>(std::floor(duration / speed * static_cast<float>(fps) + 1e-3f)) + 1;
}

std::string formatDuration(double seconds)
{
    const int total = static_cast<int>(std::lround(seconds));
    char text[32];
    snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
    return text;
}

} // namespace

void Editor::drawVideoExportPopup()
{
    if (m_openVideoExportPopup) ImGui::OpenPopup("Export Video");
    m_openVideoExportPopup = false;
    if (!ImGui::BeginPopupModal("Export Video", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const float labelWidth = 90.0f;
    const float fieldWidth = 320.0f;
    EditorStyle::propertyLabel("File", labelWidth);
    ImGui::SetNextItemWidth(fieldWidth - ImGui::CalcTextSize("Browse...").x - ImGui::GetStyle().FramePadding.x * 2 -
        ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText("##videoPath", m_videoPath, sizeof(m_videoPath));
    ImGui::SameLine();
    if (ImGui::Button("Browse..."))
        openFileDialog("ExportVideoDlg", "Export Video", ".mp4", ".",
            std::filesystem::path(m_videoPath).filename().string().c_str(), true);

    EditorStyle::propertyLabel("Resolution", labelWidth);
    ImGui::SetNextItemWidth(fieldWidth);
    if (ImGui::BeginCombo("##resolution", kResolutions[m_videoResolution].label)) {
        for (int i = 0; i < IM_ARRAYSIZE(kResolutions); ++i)
            if (ImGui::Selectable(kResolutions[i].label, i == m_videoResolution)) m_videoResolution = i;
        ImGui::EndCombo();
    }
    const int maxSize = static_cast<int>(std::max(m_renderer.maxCaptureDimension(), 16u));
    if (m_videoResolution == kCustomResolution) {
        EditorStyle::propertyLabel("Size", labelWidth);
        ImGui::SetNextItemWidth(fieldWidth);
        if (ImGui::InputInt2("##customSize", m_videoCustomSize))
            for (int& v : m_videoCustomSize) v = std::clamp(v, 16, maxSize);
        ImGui::SetItemTooltip("Width and height in pixels; odd values are rounded down to even");
    }

    EditorStyle::propertyLabel("Frame rate", labelWidth);
    ImGui::SetNextItemWidth(fieldWidth);
    char fpsLabel[16];
    snprintf(fpsLabel, sizeof(fpsLabel), "%d fps", m_videoFps);
    if (ImGui::BeginCombo("##fps", fpsLabel)) {
        for (int fps : kFrameRates) {
            snprintf(fpsLabel, sizeof(fpsLabel), "%d fps", fps);
            if (ImGui::Selectable(fpsLabel, fps == m_videoFps)) m_videoFps = fps;
        }
        ImGui::EndCombo();
    }

    EditorStyle::propertyLabel("Bitrate", labelWidth);
    ImGui::SetNextItemWidth(fieldWidth);
    ImGui::SliderInt("##bitrate", &m_videoBitrateMbps, 2, 100, "%d Mbit/s", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetItemTooltip("Higher is sharper and larger. Around 8 for 720p, 16 for 1080p, 40 for 4K.");

    const float duration = m_cameraAnimator.getDuration();
    const float speed = m_cameraAnimator.getPlaybackSpeed();
    const int frames = exportFrameCount(duration, speed, m_videoFps);
    ImGui::Spacing();
    ImGui::TextDisabled("%s of video at %.1fx speed, %d frames. Grid, path and selection are not rendered.",
        formatDuration(duration / speed).c_str(), speed, frames);
    ImGui::Separator();

    const float buttonWidth = 120.0f;
    ImGui::BeginDisabled(m_videoPath[0] == '\0' || duration <= 0.0f);
    if (ImGui::Button("Export", ImVec2(buttonWidth, 0))) {
        ImGui::CloseCurrentPopup();
        startVideoExport();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(buttonWidth, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();

    // Inside this popup so it stacks on top of it instead of closing it.
    ImGuiFileDialog* dialog = ImGuiFileDialog::Instance();
    if (dialog->Display("ExportVideoDlg", ImGuiWindowFlags_NoCollapse, kDialogSize)) {
        if (dialog->IsOk()) {
            const std::string path = toStoredPath(dialog->GetFilePathName());
            strncpy(m_videoPath, path.c_str(), sizeof(m_videoPath) - 1);
            m_videoPath[sizeof(m_videoPath) - 1] = '\0';
        }
        dialog->Close();
    }
    ImGui::EndPopup();
}

void Editor::startVideoExport()
{
    const float duration = m_cameraAnimator.getDuration();
    if (m_cameraAnimator.getPath().keyframes.size() < 2 || duration <= 0.0f) {
        setStatus("Video export needs a camera path with at least two keyframes", true);
        return;
    }

    const ResolutionPreset& preset = kResolutions[m_videoResolution];
    const int width = evenAtLeast(preset.width > 0 ? preset.width : m_videoCustomSize[0], 16);
    const int height = evenAtLeast(preset.height > 0 ? preset.height : m_videoCustomSize[1], 16);

    std::filesystem::path path(m_videoPath);
    if (path.extension() != ".mp4") path.replace_extension(".mp4");
    std::error_code ignored;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ignored);

    VideoExport& job = m_videoExport;
    job.path = path.string();
    job.width = static_cast<uint32_t>(width);
    job.height = static_cast<uint32_t>(height);
    job.fps = static_cast<uint32_t>(m_videoFps);
    job.speed = m_cameraAnimator.getPlaybackSpeed();
    job.frame = 0;
    job.frameCount = exportFrameCount(duration, job.speed, m_videoFps);

    if (!m_renderer.beginCapture(job.width, job.height)) {
        setStatus("Cannot render video frames at " + std::to_string(width) + " x " + std::to_string(height), true);
        return;
    }
    VideoEncoder::Settings settings;
    settings.path = job.path;
    settings.width = job.width;
    settings.height = job.height;
    settings.fps = job.fps;
    settings.bitrate = static_cast<uint32_t>(m_videoBitrateMbps) * 1'000'000u;
    std::string error;
    if (!job.encoder.open(settings, error)) {
        m_renderer.endCapture();
        setStatus("Video export failed: " + error, true);
        return;
    }

    // Playback would move the editor camera underneath the export.
    m_cameraAnimator.stop();
    job.active = true;
    job.startTime = ImGui::GetTime();
    setStatus("Exporting video to " + job.path + "...");
}

FrameInput Editor::videoFrameInput(int frame)
{
    const VideoExport& job = m_videoExport;
    const float time = std::min(static_cast<float>(frame) * job.speed / static_cast<float>(job.fps),
        m_cameraAnimator.getDuration());
    const CameraState state = m_cameraAnimator.getStateAtTime(time);
    Camera camera = m_camera;
    camera.position = state.position;
    camera.yaw = state.yaw;
    camera.pitch = state.pitch;

    const float width = static_cast<float>(job.width);
    const float height = static_cast<float>(job.height);
    FrameInput input;
    input.models = &m_models;
    input.view = getView(camera);
    input.proj = getProjection(width, height, kCameraNearPlane, m_settings.viewDistance);
    input.cameraPosition = camera.position;
    input.viewport = { 0.0f, 0.0f, width, height };
    input.windowWidth = width;
    input.windowHeight = height;
    input.time = time;
    return input;
}

void Editor::updateVideoExport()
{
    VideoExport& job = m_videoExport;
    if (!job.active)
        return;
    const uint64_t start = SDL_GetTicksNS();
    std::string error;
    while (job.frame < job.frameCount) {
        if (!m_renderer.captureFrame(videoFrameInput(job.frame), job.pixels)) {
            finishVideoExport(false, "rendering frame " + std::to_string(job.frame) + " failed");
            return;
        }
        if (!job.encoder.writeFrame(job.pixels, error)) {
            finishVideoExport(false, error);
            return;
        }
        ++job.frame;
        if (SDL_GetTicksNS() - start > kExportBudgetNs)
            break;
    }
    if (job.frame >= job.frameCount)
        finishVideoExport(false);
}

void Editor::finishVideoExport(bool cancelled, const std::string& error)
{
    VideoExport& job = m_videoExport;
    if (!job.active)
        return;
    std::string finishError = error;
    if (cancelled || !finishError.empty())
        job.encoder.abort();
    else
        job.encoder.finish(finishError);
    m_renderer.endCapture();
    job.active = false;
    job.pixels.clear();
    job.pixels.shrink_to_fit();

    if (cancelled)
        setStatus("Video export cancelled");
    else if (!finishError.empty())
        setStatus("Video export failed: " + finishError, true);
    else
        setStatus("Video saved: " + job.path + " (" + std::to_string(job.frameCount) + " frames, " +
            std::to_string(job.width) + " x " + std::to_string(job.height) + ", " +
            formatDuration(ImGui::GetTime() - job.startTime) + " to render)");
}

void Editor::drawVideoExportProgress()
{
    const VideoExport& job = m_videoExport;
    if (job.active && !ImGui::IsPopupOpen("Exporting Video"))
        ImGui::OpenPopup("Exporting Video");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Exporting Video", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove))
        return;
    if (!job.active) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextUnformatted(job.path.c_str());
    ImGui::TextDisabled("%u x %u, %u fps", job.width, job.height, job.fps);
    const float progress = job.frameCount > 0 ? static_cast<float>(job.frame) / job.frameCount : 0.0f;
    char text[64];
    snprintf(text, sizeof(text), "Frame %d / %d", job.frame, job.frameCount);
    ImGui::ProgressBar(progress, ImVec2(360.0f, 0), text);

    const double elapsed = ImGui::GetTime() - job.startTime;
    if (job.frame > 0) {
        const double remaining = elapsed / job.frame * (job.frameCount - job.frame);
        ImGui::TextDisabled("Elapsed %s, about %s left", formatDuration(elapsed).c_str(),
            formatDuration(remaining).c_str());
    }
    else {
        ImGui::TextDisabled("Starting...");
    }
    ImGui::Spacing();
    if (ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0)))
        finishVideoExport(true);
    ImGui::EndPopup();
}
