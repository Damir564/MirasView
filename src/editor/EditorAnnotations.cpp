#include "Editor.h"
#include <cstring>
#include "EditorStyle.h"
#include "engine/ModelManager.h"

void Editor::drawAnnotationPopup()
{
    if (m_openAnnotationPopup) {
        ImGui::OpenPopup("Add Annotation");
        m_openAnnotationPopup = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Add Annotation", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const auto& instances = m_models.getInstances();
    if (validInstance(m_annotationTargetInstance) && instances[m_annotationTargetInstance].ifcScene) {
        const IfcScene& scene = *instances[m_annotationTargetInstance].ifcScene;
        auto it = scene.elements.find(m_annotationTargetGuid);
        if (it != scene.elements.end()) {
            const IfcElement& element = it->second;
            ImGui::TextColored(EditorStyle::kHighlight, "%s", element.name.empty() ? element.type.c_str() : element.name.c_str());
            ImGui::TextDisabled("%s   %s", element.type.c_str(), element.guid.c_str());
        }
    }
    ImGui::Spacing();
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(420);
    const bool enterPressed = ImGui::InputTextWithHint("##annottext", "Annotation text", m_annotationText,
        sizeof(m_annotationText), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();

    if (ImGui::Button("Add", ImVec2(120, 0)) || enterPressed) {
        addAnnotationFromPopup();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void Editor::addAnnotationFromPopup()
{
    if (strlen(m_annotationText) == 0 || !validInstance(m_annotationTargetInstance))
        return;
    const ModelInstance& instance = m_models.getInstances()[m_annotationTargetInstance];
    Annotation annotation;
    annotation.text = m_annotationText;
    annotation.ifcGuid = m_annotationTargetGuid;
    annotation.instanceIndex = m_annotationTargetInstance;
    annotation.worldPosition = instance.position;
    GPUModel* model = m_models.getModel(instance.modelIndex);
    float radius = 0.0f;
    if (instance.ifcScene)
        ifcElementWorldBounds(model, *instance.ifcScene, m_annotationTargetGuid, instance, annotation.worldPosition, radius);
    m_annotations.push_back(annotation);
    setStatus("Annotation added");
}

void Editor::drawViewportAnnotations()
{
    const auto& instances = m_models.getInstances();
    const glm::mat4 viewProj = sceneProjection() * getView(m_camera);

    // Background draw list: labels stay behind the editor panels.
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    drawList->PushClipRect(ImVec2(m_sceneView.x, m_sceneView.y),
        ImVec2(m_sceneView.x + m_sceneView.width, m_sceneView.y + m_sceneView.height), true);
    for (const auto& annotation : m_annotations) {
        if (!validInstance(annotation.instanceIndex) || !instances[annotation.instanceIndex].visible) continue;

        const glm::vec4 clipPos = viewProj * glm::vec4(annotation.worldPosition, 1.0f);
        if (clipPos.w <= 0.0f) continue;
        const glm::vec3 ndc = glm::vec3(clipPos) / clipPos.w;
        if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f) continue;

        const float screenX = m_sceneView.x + (ndc.x * 0.5f + 0.5f) * m_sceneView.width;
        const float screenY = m_sceneView.y + (ndc.y * 0.5f + 0.5f) * m_sceneView.height;
        const float dist = glm::distance(m_camera.position, annotation.worldPosition);
        const float alpha = glm::clamp(1.0f - (dist - 50.0f) / 100.0f, 0.1f, 1.0f);

        const ImVec2 textSize = ImGui::CalcTextSize(annotation.text.c_str());
        const float padding = 6.0f;
        const ImVec2 boxMin(screenX - padding, screenY - padding);
        const ImVec2 boxMax(screenX + textSize.x + padding, screenY + textSize.y + padding);
        const ImU32 bgColor = IM_COL32(30, 30, 30, static_cast<int>(200 * alpha));
        const ImU32 borderColor = IM_COL32(255, 200, 50, static_cast<int>(255 * alpha));
        const ImU32 textColor = IM_COL32(255, 255, 255, static_cast<int>(255 * alpha));

        drawList->AddRectFilled(boxMin, boxMax, bgColor, 4.0f);
        drawList->AddRect(boxMin, boxMax, borderColor, 4.0f, 0, 1.5f);
        drawList->AddLine(ImVec2(screenX + textSize.x * 0.5f, boxMax.y),
            ImVec2(screenX + textSize.x * 0.5f, boxMax.y + 10.0f), borderColor, 1.5f);
        drawList->AddCircleFilled(ImVec2(screenX + textSize.x * 0.5f, boxMax.y + 10.0f), 3.0f, borderColor);
        drawList->AddText(ImVec2(screenX, screenY), textColor, annotation.text.c_str());
    }
    drawList->PopClipRect();
}

void Editor::drawAnnotationsPanel()
{
    if (ImGui::Begin("Annotations", &m_showAnnotationsPanel)) {
        ImGui::Checkbox("Show in viewport", &m_showAnnotations);
        ImGui::SameLine();
        ImGui::BeginDisabled(m_ifcSelectionKind != IfcSelectionKind::Element);
        if (ImGui::Button("Add to Selected (M)")) requestAnnotation();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(m_annotations.empty());
        if (ImGui::Button("Clear All")) m_annotations.clear();
        ImGui::EndDisabled();
        ImGui::Separator();

        if (m_annotations.empty()) {
            ImGui::TextDisabled("No annotations yet.");
            ImGui::TextWrapped("Select an IFC element (in the viewport or Hierarchy) and press M to pin a note to it.");
        }
        else {
            drawAnnotationTable();
        }
    }
    ImGui::End();
}

void Editor::drawAnnotationTable()
{
    if (!ImGui::BeginTable("##annotations", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY))
        return;
    auto& instances = m_models.getInstances();
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Note", ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableSetupColumn("Element", ImGuiTableColumnFlags_WidthStretch, 0.5f);
    ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 90.0f);
    ImGui::TableHeadersRow();
    int deferredRemove = -1;
    for (int i = 0; i < static_cast<int>(m_annotations.size()); ++i) {
        const Annotation& annotation = m_annotations[i];
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", annotation.text.c_str());
        ImGui::TableNextColumn();
        IfcElement* target = nullptr;
        if (validInstance(annotation.instanceIndex) && instances[annotation.instanceIndex].ifcScene) {
            auto& elements = instances[annotation.instanceIndex].ifcScene->elements;
            auto it = elements.find(annotation.ifcGuid);
            if (it != elements.end()) target = &it->second;
        }
        if (target) ImGui::TextDisabled("%s", target->name.empty() ? target->type.c_str() : target->name.c_str());
        else ImGui::TextDisabled("(missing)");
        ImGui::TableNextColumn();
        ImGui::BeginDisabled(!target);
        if (ImGui::SmallButton("Go to")) {
            selectIfcElement(annotation.instanceIndex, *instances[annotation.instanceIndex].ifcScene, annotation.ifcGuid);
            m_selectionChangedFromViewport = true;
            focusOnInstance(annotation.instanceIndex);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete")) deferredRemove = i;
        ImGui::PopID();
    }
    ImGui::EndTable();
    if (deferredRemove >= 0) m_annotations.erase(m_annotations.begin() + deferredRemove);
}
