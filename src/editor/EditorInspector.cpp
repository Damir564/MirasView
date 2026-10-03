#include "Editor.h"
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <map>
#include "EditorStyle.h"
#include "engine/ModelManager.h"

namespace {
constexpr ImGuiTableFlags kInfoTableFlags =
    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV;
}

void Editor::drawInspector()
{
    if (ImGui::Begin("Inspector", &m_showInspector)) {
        if (!hasSelection()) {
            ImGui::Spacing();
            ImGui::TextDisabled("Nothing selected.");
            ImGui::Spacing();
            ImGui::TextWrapped("Click an object in the viewport or in the Hierarchy to see and edit its properties here.");
        }
        else {
            const int index = m_gizmo.selectedInstance;
            ModelInstance& instance = m_models.getInstances()[index];
            drawInspectorHeader(instance, m_models.getModel(instance.modelIndex));
            if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen))
                drawTransformSection(index);
            // The transform buttons may have deleted or replaced the selection.
            if (hasSelection() && m_gizmo.selectedInstance == index &&
                ImGui::CollapsingHeader("Appearance", ImGuiTreeNodeFlags_DefaultOpen))
                drawAppearanceSection(index);
            drawIfcSelectionDetails();
        }
    }
    ImGui::End();
}

void Editor::drawInspectorHeader(ModelInstance& instance, const GPUModel* model)
{
    ImGui::Checkbox("##visible", &instance.visible);
    ImGui::SetItemTooltip("Visible");
    ImGui::SameLine();
    char nameBuf[256];
    strncpy(nameBuf, instance.name.c_str(), sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
        instance.name = nameBuf;
    if (model) {
        ImGui::TextDisabled("Model: %s", model->name.c_str());
        ImGui::TextDisabled("%s vertices, %s triangles",
            formatCount(model->vertexCount).c_str(), formatCount(model->indexCount / 3).c_str());
    }
    ImGui::Spacing();
}

void Editor::drawTransformSection(int instanceIndex)
{
    ModelInstance& instance = m_models.getInstances()[instanceIndex];
    EditorStyle::vec3Control("Position", &instance.position.x, 0.0f, 0.1f);
    EditorStyle::vec3Control("Rotation", &instance.rotation.x, 0.0f, 0.5f);
    if (EditorStyle::vec3Control("Scale", &instance.scale.x, 1.0f, 0.01f, 0.01f, 1000.0f))
        instance.scale = glm::max(instance.scale, glm::vec3(0.01f));

    ImGui::Spacing();
    const float buttonWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2) / 3.0f;
    if (ImGui::Button("Focus", ImVec2(buttonWidth, 0))) focusOnInstance(instanceIndex);
    ImGui::SameLine();
    if (ImGui::Button("Duplicate", ImVec2(buttonWidth, 0))) duplicateInstance(instanceIndex);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorStyle::kDanger);
    const bool deletePressed = ImGui::Button("Delete", ImVec2(buttonWidth, 0));
    ImGui::PopStyleColor();
    if (deletePressed) deleteInstance(instanceIndex);
}

void Editor::drawAppearanceSection(int instanceIndex)
{
    ModelInstance& instance = m_models.getInstances()[instanceIndex];
    const ImGuiStyle& style = ImGui::GetStyle();
    const float extraWidth = ImGui::CalcTextSize("Reset").x + ImGui::CalcTextSize("Color").x +
        style.FramePadding.x * 2 + style.ItemSpacing.x + style.ItemInnerSpacing.x;
    ImGui::SetNextItemWidth(std::max(ImGui::GetContentRegionAvail().x - extraWidth, 60.0f));
    ImGui::ColorEdit3("Color", &instance.color.x);
    ImGui::SetItemTooltip("Color (tint multiplied into the model's base color)");
    ImGui::SameLine();
    if (ImGui::Button("Reset")) instance.color = glm::vec3(1.0f);
    ImGui::SetItemTooltip("Reset the color to white");

    if (const GPUModel* model = m_models.getModel(instance.modelIndex)) {
        ImGui::Spacing();
        drawMaterialEditor(instance, *model);
    }
}

// Material slots come from the model and are shared by its instances, so edits are stored on the
// instance as overrides. Submeshes the model left without a material are slot -1 and can get one added.
void Editor::drawMaterialEditor(ModelInstance& instance, const GPUModel& model)
{
    const int slotCount = static_cast<int>(model.materialNames.size());
    const bool hasUnassigned = model.unassignedMaterial.has_value();
    if (slotCount == 0 && !hasUnassigned)
        return;

    const int instanceIndex = m_gizmo.selectedInstance;
    const bool slotValid = m_materialSlot < 0 ? hasUnassigned : m_materialSlot < slotCount;
    if (m_materialSlotInstance != instanceIndex || !slotValid) {
        m_materialSlotInstance = instanceIndex;
        m_materialSlot = slotCount > 0 ? 0 : -1;
    }

    const auto slotLabel = [&](int slot) {
        std::string label = slot < 0 ? "(no material)" : model.materialNames[slot];
        if (instance.findMaterial(slot)) label += slot < 0 ? " (added)" : " (edited)";
        return label;
    };
    const float labelWidth = ImGui::CalcTextSize("Roughness").x + ImGui::GetStyle().ItemInnerSpacing.x;
    ImGui::PushItemWidth(std::max(ImGui::GetContentRegionAvail().x - labelWidth, 60.0f));

    if (slotCount + (hasUnassigned ? 1 : 0) > 1) {
        if (ImGui::BeginCombo("Material", slotLabel(m_materialSlot).c_str(), ImGuiComboFlags_HeightLarge)) {
            const int first = hasUnassigned ? -1 : 0;
            for (int slot = first; slot < slotCount; ++slot) {
                ImGui::PushID(slot);
                const bool selected = slot == m_materialSlot;
                if (ImGui::Selectable(slotLabel(slot).c_str(), selected)) m_materialSlot = slot;
                if (selected) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Material slot of this model to edit");
    }
    else if (m_materialSlot >= 0) {
        ImGui::TextDisabled("Material: %s", model.materialNames[m_materialSlot].c_str());
    }

    const int slot = m_materialSlot;
    const MaterialOverride* current = instance.findMaterial(slot);
    const float buttonWidth = ImGui::GetContentRegionAvail().x;
    if (slot < 0 && !current) {
        ImGui::TextDisabled("This object has no material.");
        if (ImGui::Button("Add Material", ImVec2(buttonWidth, 0))) {
            const Material& base = *model.unassignedMaterial;
            instance.materials[slot] = MaterialOverride{ glm::vec3(base.baseColorFactor), base.metallicFactor,
                base.roughnessFactor };
            setStatus("Added a material to " + instance.name);
        }
        ImGui::PopItemWidth();
        return;
    }

    MaterialOverride values;
    if (current) {
        values = *current;
    }
    else {
        const Material& base = model.slotMaterials[slot];
        values = { glm::vec3(base.baseColorFactor), base.metallicFactor, base.roughnessFactor };
    }
    bool changed = ImGui::ColorEdit3("Base Color", &values.baseColor.x);
    ImGui::SetItemTooltip("Base color of the material (textures are multiplied by it)");
    changed |= ImGui::SliderFloat("Metallic", &values.metallic, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    changed |= ImGui::SliderFloat("Roughness", &values.roughness, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (changed)
        instance.materials[slot] = values;
    ImGui::PopItemWidth();

    if (current || changed) {
        const char* label = slot < 0 ? "Remove Material" : "Revert to Model Material";
        if (slot < 0) ImGui::PushStyleColor(ImGuiCol_ButtonHovered, EditorStyle::kDanger);
        const bool pressed = ImGui::Button(label, ImVec2(buttonWidth, 0));
        if (slot < 0) ImGui::PopStyleColor();
        if (pressed) instance.materials.erase(slot);
        ImGui::SetItemTooltip(slot < 0 ? "Remove the material added to this object"
                                       : "Discard this object's changes to the material");
    }
}

void Editor::drawIfcSelectionDetails()
{
    // Transform buttons above may have deleted or replaced the selection this frame.
    if (!hasSelection() || m_ifcSelectionKind == IfcSelectionKind::None ||
        m_ifcSelectionInstance != m_gizmo.selectedInstance ||
        !m_models.getInstances()[m_gizmo.selectedInstance].ifcScene)
        return;
    IfcScene& scene = *m_models.getInstances()[m_gizmo.selectedInstance].ifcScene;
    const auto* properties = m_ifcSelectionKind == IfcSelectionKind::Element
        ? drawIfcElementSection(scene)
        : drawIfcSpatialSection(scene);
    if (properties && !properties->empty())
        drawIfcProperties(*properties);
}

const std::unordered_map<std::string, std::string>* Editor::drawIfcElementSection(IfcScene& scene)
{
    auto it = scene.elements.find(m_selectedIfcGuid);
    if (it == scene.elements.end())
        return nullptr;
    IfcElement& element = it->second;
    if (!ImGui::CollapsingHeader("IFC Element", ImGuiTreeNodeFlags_DefaultOpen))
        return &element.data;

    if (ImGui::BeginTable("##ifcElementInfo", 2, kInfoTableFlags)) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        EditorStyle::keyValueRow("Name", element.name.c_str());
        EditorStyle::keyValueRow("Class", element.type.c_str());
        EditorStyle::keyValueRow("GUID", element.guid.c_str());
        EditorStyle::keyValueRow("Tag", element.tag.c_str());
        EditorStyle::keyValueRow("Storey", element.storey.c_str());
        EditorStyle::keyValueRow("Object type", element.objectType.c_str());
        if (element.typeInfo) {
            EditorStyle::keyValueRow("Type name", element.typeInfo->name.c_str());
            EditorStyle::keyValueRow("Type class", element.typeInfo->type.c_str());
            EditorStyle::keyValueRow("Predefined", element.typeInfo->predefinedType.c_str());
        }
        ImGui::EndTable();
    }

    const float buttonWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
    if (ImGui::Button("Isolate", ImVec2(buttonWidth, 0))) isolateIfcElement(scene, element.guid);
    ImGui::SameLine();
    if (ImGui::Button("Show All", ImVec2(buttonWidth, 0))) showAllIfc(scene);
    if (ImGui::Button("Add Annotation (M)", ImVec2(buttonWidth, 0))) requestAnnotation();
    ImGui::SameLine();
    if (ImGui::Button("Copy GUID", ImVec2(buttonWidth, 0))) {
        ImGui::SetClipboardText(element.guid.c_str());
        setStatus("GUID copied to clipboard");
    }
    drawSameNameCounts(element);
    return &element.data;
}

void Editor::drawSameNameCounts(const IfcElement& element)
{
    SameNameCount& count = m_sameNameCount;
    if (count.guid != element.guid || count.instance != m_gizmo.selectedInstance) {
        count.guid = element.guid;
        count.instance = m_gizmo.selectedInstance;
        count.inModel = count.inScene = 0;
        const auto& instances = m_models.getInstances();
        if (!element.name.empty()) {
            for (int k = 0; k < static_cast<int>(instances.size()); ++k) {
                if (!instances[k].ifcScene) continue;
                for (const auto& [guid, other] : instances[k].ifcScene->elements) {
                    if (other.name != element.name) continue;
                    ++count.inScene;
                    if (k == m_gizmo.selectedInstance) ++count.inModel;
                }
            }
        }
    }
    ImGui::TextDisabled("Elements with this name: %s in this model, %s in the scene",
        formatCount(count.inModel).c_str(), formatCount(count.inScene).c_str());
}

const std::unordered_map<std::string, std::string>* Editor::drawIfcSpatialSection(IfcScene& scene)
{
    auto it = scene.spatial.find(m_selectedIfcGuid);
    if (it == scene.spatial.end())
        return nullptr;
    IfcSpatialNode& node = it->second;
    if (!ImGui::CollapsingHeader("IFC Spatial Structure", ImGuiTreeNodeFlags_DefaultOpen))
        return &node.data;

    if (ImGui::BeginTable("##ifcSpatialInfo", 2, kInfoTableFlags)) {
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 100.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        EditorStyle::keyValueRow("Name", node.name.c_str());
        EditorStyle::keyValueRow("Long name", node.longName.c_str());
        EditorStyle::keyValueRow("Class", node.type.c_str());
        EditorStyle::keyValueRow("GUID", node.guid.c_str());
        EditorStyle::keyValueRow("Sub-spaces", std::to_string(node.childSpatialGuids.size()).c_str());
        EditorStyle::keyValueRow("Elements", std::to_string(node.elementGuids.size()).c_str());
        ImGui::EndTable();
    }
    const float buttonWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) / 2.0f;
    if (ImGui::Button("Show Only This", ImVec2(buttonWidth, 0))) showOnlyIfcBranch(scene, node.guid);
    ImGui::SameLine();
    if (ImGui::Button("Show All", ImVec2(buttonWidth, 0))) showAllIfc(scene);
    return &node.data;
}

// Property keys are "PsetName.Property" and are grouped by set, like BIM tools do.
void Editor::drawIfcProperties(const std::unordered_map<std::string, std::string>& properties)
{
    char header[64];
    snprintf(header, sizeof(header), "Properties (%zu)###IfcProperties", properties.size());
    if (!ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
        return;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##propertyFilter", "Filter properties...",
            m_propertyFilter.InputBuf, IM_ARRAYSIZE(m_propertyFilter.InputBuf)))
        m_propertyFilter.Build();

    std::map<std::string, std::vector<std::pair<std::string, const std::string*>>> groups;
    for (const auto& [key, value] : properties) {
        if (!m_propertyFilter.PassFilter(key.c_str()) && !m_propertyFilter.PassFilter(value.c_str()))
            continue;
        const size_t dot = key.find('.');
        std::string group = dot == std::string::npos ? "General" : key.substr(0, dot);
        std::string name = dot == std::string::npos ? key : key.substr(dot + 1);
        groups[group].emplace_back(std::move(name), &value);
    }
    if (groups.empty())
        ImGui::TextDisabled("No properties match the filter.");
    for (auto& [group, entries] : groups) {
        std::sort(entries.begin(), entries.end());
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (!ImGui::TreeNodeEx(group.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, "%s  (%zu)", group.c_str(), entries.size()))
            continue;
        if (ImGui::BeginTable("##props", 2, kInfoTableFlags | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthStretch, 0.45f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 0.55f);
            for (const auto& [name, value] : entries) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextColored(EditorStyle::kTextDim, "%s", name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", value->c_str());
                if (ImGui::BeginPopupContextItem(name.c_str())) {
                    if (ImGui::MenuItem("Copy Value")) ImGui::SetClipboardText(value->c_str());
                    ImGui::EndPopup();
                }
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }
}
