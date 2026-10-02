#include "Editor.h"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include "EditorStyle.h"
#include "engine/ModelManager.h"
#include "engine/VulkanContext.h"

void Editor::drawStatisticsPanel()
{
    if (ImGui::Begin("Statistics", &m_showStatisticsPanel)) {
        const ImGuiIO& io = ImGui::GetIO();
        m_frameTimes[m_frameTimeOffset] = 1000.0f * io.DeltaTime;
        m_frameTimeOffset = (m_frameTimeOffset + 1) % IM_ARRAYSIZE(m_frameTimes);
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%.0f FPS  (%.2f ms)", io.Framerate, 1000.0f / std::max(io.Framerate, 0.001f));
        ImGui::PlotLines("##frameTimes", m_frameTimes, IM_ARRAYSIZE(m_frameTimes), m_frameTimeOffset, overlay,
            0.0f, 33.3f, ImVec2(-FLT_MIN, 60.0f));

        const auto& instances = m_models.getInstances();
        size_t visibleObjects = 0, sceneVertices = 0, sceneTriangles = 0, textures = 0;
        for (const auto& instance : instances) {
            if (!instance.visible) continue;
            ++visibleObjects;
            if (GPUModel* model = m_models.getModel(instance.modelIndex)) {
                sceneVertices += model->vertexCount;
                sceneTriangles += model->indexCount / 3;
            }
        }
        for (const auto& model : m_models.getModels()) textures += model->textures.size();

        if (ImGui::BeginTable("##stats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            EditorStyle::keyValueRow("Objects", (std::to_string(visibleObjects) + " visible / " + std::to_string(instances.size())).c_str());
            EditorStyle::keyValueRow("Models loaded", std::to_string(m_models.getModels().size()).c_str());
            EditorStyle::keyValueRow("Scene vertices", formatCount(sceneVertices).c_str());
            EditorStyle::keyValueRow("Scene triangles", formatCount(sceneTriangles).c_str());
            EditorStyle::keyValueRow("Textures", std::to_string(textures).c_str());
            char buf[96];
            snprintf(buf, sizeof(buf), "%.2f, %.2f, %.2f", m_camera.position.x, m_camera.position.y, m_camera.position.z);
            EditorStyle::keyValueRow("Camera position", buf);
            snprintf(buf, sizeof(buf), "yaw %.1f, pitch %.1f", m_camera.yaw, m_camera.pitch);
            EditorStyle::keyValueRow("Camera rotation", buf);
            snprintf(buf, sizeof(buf), "%u x %u px", m_renderer.framebufferWidth(), m_renderer.framebufferHeight());
            EditorStyle::keyValueRow("Window", buf);
            snprintf(buf, sizeof(buf), "%.0f x %.0f", m_sceneView.width, m_sceneView.height);
            EditorStyle::keyValueRow("Viewport", buf);
            snprintf(buf, sizeof(buf), "%s (Vulkan %u.%u, %s)", m_vulkan.vkbPhysicalDevice().properties.deviceName,
                VK_API_VERSION_MAJOR(m_vulkan.apiVersion()), VK_API_VERSION_MINOR(m_vulkan.apiVersion()),
                vulkanBackendName(m_vulkan.backend()));
            EditorStyle::keyValueRow("GPU", buf);
            ImGui::EndTable();
        }
    }
    ImGui::End();
}
