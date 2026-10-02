#include "Editor.h"
#include <algorithm>
#include <cfloat>
#include "engine/ModelManager.h"

void Editor::drawHierarchy()
{
    if (ImGui::Begin("Hierarchy", &m_showHierarchy)) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::InputTextWithHint("##hierarchySearch", "Search objects and IFC elements...",
                m_hierarchyFilter.InputBuf, IM_ARRAYSIZE(m_hierarchyFilter.InputBuf)))
            m_hierarchyFilter.Build();
        if (ImGui::CollapsingHeader("Models", ImGuiTreeNodeFlags_DefaultOpen))
            drawModelList();
        if (ImGui::CollapsingHeader("Scene", ImGuiTreeNodeFlags_DefaultOpen))
            drawSceneTree();

        if (ImGui::BeginPopupContextWindow("HierarchyEmptyMenu",
                ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            if (ImGui::BeginMenu("Create")) {
                if (ImGui::MenuItem("Cube")) addCube();
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Import Model...")) importModelDialog();
            ImGui::EndPopup();
        }
    }
    ImGui::End();
}

void Editor::drawModelList()
{
    const auto& models = m_models.getModels();
    const auto& tasks = m_models.getLoadingTasks();
    int deferredUnload = -1;
    for (size_t i = 0; i < models.size(); ++i) {
        const auto& model = models[i];
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::Selectable(model->name.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick) &&
            ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            addModelToScene(i, false);
        if (ImGui::BeginItemTooltip()) {
            ImGui::TextUnformatted(model->sourcePath.c_str());
            ImGui::TextDisabled("%s vertices, %s triangles, %zu textures",
                formatCount(model->vertexCount).c_str(), formatCount(model->indexCount / 3).c_str(),
                model->textures.size());
            ImGui::TextDisabled("Double-click to add to the scene");
            ImGui::EndTooltip();
        }
        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem("Add to Scene")) addModelToScene(i, false);
            if (ImGui::MenuItem("Add at Origin")) addModelToScene(i, true);
            ImGui::Separator();
            if (ImGui::MenuItem("Unload")) deferredUnload = static_cast<int>(i);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    for (const auto& task : tasks) {
        const char* state = task.state == LoadingState::LoadingCPU ? "parsing"
            : task.state == LoadingState::UploadingGPU ? "uploading"
            : task.state == LoadingState::Failed ? "failed" : "queued";
        ImGui::TextDisabled("%s  (%s...)", task.name.c_str(), state);
    }
    if (models.empty() && tasks.empty())
        ImGui::TextDisabled("No models loaded.");
    if (ImGui::Button("Import Model...", ImVec2(-FLT_MIN, 0))) importModelDialog();
    if (deferredUnload >= 0) unloadModel(static_cast<size_t>(deferredUnload));
}

// A viewport pick expands the tree down to the picked element. Returns the instance to force open, or -1.
int Editor::expandTreeToViewportSelection()
{
    if (!m_selectionChangedFromViewport || m_selectedIfcGuid.empty() || !validInstance(m_ifcSelectionInstance))
        return -1;
    IfcScene& scene = *m_models.getInstances()[m_ifcSelectionInstance].ifcScene;
    m_openSpatialGuids.clear();
    std::string guid;
    if (m_ifcSelectionKind == IfcSelectionKind::Element) {
        auto it = scene.elements.find(m_selectedIfcGuid);
        if (it != scene.elements.end()) guid = it->second.parentSpatialGuid;
    }
    else {
        guid = m_selectedIfcGuid;
    }
    while (!guid.empty()) {
        m_openSpatialGuids.insert(guid);
        auto it = scene.spatial.find(guid);
        if (it == scene.spatial.end()) break;
        guid = it->second.parentGuid;
    }
    m_selectionChangedFromViewport = false;
    return m_ifcSelectionInstance;
}

void Editor::drawSceneTree()
{
    auto& instances = m_models.getInstances();
    // Compact rows: small visibility checkboxes and tight spacing, like editor hierarchies.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(3, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 3));
    if (instances.empty())
        ImGui::TextDisabled("The scene is empty.\nDouble-click a model above to add it.");

    if (!validInstance(m_renamingInstance))
        m_renamingInstance = -1;
    const int forceOpenInstance = expandTreeToViewportSelection();
    m_searchCaches.resize(instances.size());

    HierarchyActions actions;
    for (int i = 0; i < static_cast<int>(instances.size()); ++i)
        drawInstanceNode(i, forceOpenInstance, actions);

    // Keep the path open for a few frames so the tree has time to scroll to the element.
    if (forceOpenInstance >= 0) m_openPathFramesLeft = 3;
    if (!m_openSpatialGuids.empty() && --m_openPathFramesLeft <= 0) m_openSpatialGuids.clear();

    if (actions.focusIndex >= 0) {
        if (m_gizmo.selectedInstance != actions.focusIndex) selectInstance(actions.focusIndex);
        focusOnInstance(actions.focusIndex);
    }
    if (actions.duplicateIndex >= 0) duplicateInstance(actions.duplicateIndex);
    if (actions.createCube) addCube();
    if (actions.deleteIndex >= 0) deleteInstance(actions.deleteIndex);
    ImGui::PopStyleVar(2);
}

// Search results are shown as a flat list per object (like Unity's hierarchy search).
const std::vector<std::string>* Editor::updateSearchCache(int instanceIndex)
{
    auto& instance = m_models.getInstances()[instanceIndex];
    if (!instance.ifcScene)
        return nullptr;
    IfcScene& scene = *instance.ifcScene;
    SearchCache& cache = m_searchCaches[instanceIndex];
    if (cache.filter != m_hierarchyFilter.InputBuf || cache.elementCount != scene.elements.size()) {
        cache.filter = m_hierarchyFilter.InputBuf;
        cache.elementCount = scene.elements.size();
        cache.guids.clear();
        for (const auto& [guid, element] : scene.elements)
            if (m_hierarchyFilter.PassFilter(element.name.c_str()) || m_hierarchyFilter.PassFilter(element.type.c_str()))
                cache.guids.push_back(guid);
        std::sort(cache.guids.begin(), cache.guids.end(), [&](const std::string& a, const std::string& b) {
            return scene.elements[a].name < scene.elements[b].name;
        });
    }
    return &cache.guids;
}

void Editor::drawInstanceNode(int instanceIndex, int forceOpenInstance, HierarchyActions& actions)
{
    auto& instance = m_models.getInstances()[instanceIndex];
    const bool filtering = m_hierarchyFilter.IsActive();
    const std::vector<std::string>* matches = nullptr;
    if (filtering) {
        matches = updateSearchCache(instanceIndex);
        if (!m_hierarchyFilter.PassFilter(instance.name.c_str()) && (!matches || matches->empty()))
            return;
    }

    ImGui::PushID(instanceIndex);
    ImGui::Checkbox("##visible", &instance.visible);
    ImGui::SetItemTooltip(instance.visible ? "Hide object" : "Show object");
    ImGui::SameLine();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!instance.ifcScene) flags |= ImGuiTreeNodeFlags_Leaf;
    if (m_gizmo.selectedInstance == instanceIndex && m_ifcSelectionKind == IfcSelectionKind::None)
        flags |= ImGuiTreeNodeFlags_Selected;
    if (forceOpenInstance == instanceIndex || (matches && !matches->empty()))
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);

    if (m_renamingInstance == instanceIndex) {
        drawRenameField(instanceIndex);
        ImGui::PopID();
        return;
    }

    const bool open = ImGui::TreeNodeEx("##instance", flags, "%s", instance.name.c_str());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        selectInstance(instanceIndex);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        actions.focusIndex = instanceIndex;
    drawInstanceContextMenu(instanceIndex, actions);
    if (instance.ifcScene) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", formatCount(instance.ifcScene->elements.size()).c_str());
    }

    if (open) {
        if (instance.ifcScene) {
            IfcScene& scene = *instance.ifcScene;
            if (matches) {
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(matches->size()));
                while (clipper.Step()) {
                    for (int m = clipper.DisplayStart; m < clipper.DisplayEnd; ++m) {
                        auto it = scene.elements.find((*matches)[m]);
                        if (it != scene.elements.end())
                            drawElementRow(instanceIndex, scene, it->second);
                    }
                }
            }
            else {
                for (const auto& rootGuid : scene.roots)
                    drawSpatialNode(instanceIndex, scene, rootGuid);
            }
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

// Inline rename: Enter or clicking elsewhere commits, Esc cancels. Returns true when editing ended.
bool Editor::drawRenameField(int instanceIndex)
{
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (m_renameFocusPending) {
        ImGui::SetKeyboardFocusHere();
        m_renameFocusPending = false;
    }
    const bool entered = ImGui::InputText("##rename", m_renameBuffer, sizeof(m_renameBuffer),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    bool commit = entered;
    bool finished = entered;
    if (!entered && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        finished = true;
    }
    else if (!entered && ImGui::IsItemDeactivated()) {
        commit = finished = true;
    }
    else if (!ImGui::IsItemActive() && !m_renameFocusPending && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
             !ImGui::IsItemHovered()) {
        // Focus never arrived (e.g. the row was scrolled away); treat an outside click as commit.
        commit = finished = true;
    }
    if (commit) {
        std::string name = m_renameBuffer;
        const size_t first = name.find_first_not_of(" \t");
        const size_t last = name.find_last_not_of(" \t");
        name = first == std::string::npos ? std::string() : name.substr(first, last - first + 1);
        if (!name.empty() && validInstance(instanceIndex)) {
            m_models.getInstances()[instanceIndex].name = name;
            setStatus("Renamed to " + name);
        }
    }
    if (finished)
        m_renamingInstance = -1;
    return finished;
}

void Editor::drawInstanceContextMenu(int instanceIndex, HierarchyActions& actions)
{
    if (!ImGui::BeginPopupContextItem("InstanceContextMenu"))
        return;
    auto& instance = m_models.getInstances()[instanceIndex];
    if (ImGui::MenuItem("Focus", "F")) actions.focusIndex = instanceIndex;
    if (ImGui::MenuItem("Rename", "F2")) {
        selectInstance(instanceIndex);
        beginRename(instanceIndex);
    }
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) actions.duplicateIndex = instanceIndex;
    if (ImGui::MenuItem("Delete", "Del")) actions.deleteIndex = instanceIndex;
    if (instance.ifcScene) {
        ImGui::Separator();
        if (ImGui::MenuItem("Show All Elements")) showAllIfc(*instance.ifcScene);
        if (ImGui::MenuItem("Hide All Elements")) hideAllIfc(*instance.ifcScene);
    }
    ImGui::Separator();
    if (ImGui::BeginMenu("Create")) {
        if (ImGui::MenuItem("Cube")) actions.createCube = true;
        ImGui::EndMenu();
    }
    ImGui::EndPopup();
}

void Editor::drawElementRow(int instanceIndex, IfcScene& scene, IfcElement& element)
{
    ImGui::PushID(element.guid.c_str());
    if (ImGui::Checkbox("##ev", &element.visible))
        scene.syncVisibilityCache();
    ImGui::SameLine();
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
        ImGuiTreeNodeFlags_SpanAvailWidth;
    if (element.selected) flags |= ImGuiTreeNodeFlags_Selected;
    const char* name = element.name.empty() ? element.type.c_str() : element.name.c_str();
    ImGui::TreeNodeEx("##elem", flags, "%s", name);
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (clicked || (ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter)))
        selectIfcElement(instanceIndex, scene, element.guid);
    if (doubleClicked)
        focusOnInstance(instanceIndex);
    if (element.selected && m_ifcSelectionKind == IfcSelectionKind::Element && !m_openSpatialGuids.empty()) {
        ImGui::SetScrollHereY(0.5f);
        m_openSpatialGuids.clear();
    }
    drawElementContextMenu(instanceIndex, scene, element);
    if (ImGui::BeginItemTooltip()) {
        ImGui::Text("%s", name);
        ImGui::TextDisabled("%s", element.type.c_str());
        if (!element.storey.empty()) ImGui::TextDisabled("Storey: %s", element.storey.c_str());
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", element.type.c_str());
    ImGui::PopID();
}

void Editor::drawElementContextMenu(int instanceIndex, IfcScene& scene, IfcElement& element)
{
    if (!ImGui::BeginPopupContextItem("ElementContextMenu"))
        return;
    if (ImGui::MenuItem("Select")) selectIfcElement(instanceIndex, scene, element.guid);
    if (ImGui::MenuItem("Focus", "F")) {
        selectIfcElement(instanceIndex, scene, element.guid);
        focusOnInstance(instanceIndex);
    }
    if (ImGui::MenuItem("Add Annotation...", "M")) {
        selectIfcElement(instanceIndex, scene, element.guid);
        requestAnnotation();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Isolate")) isolateIfcElement(scene, element.guid);
    if (ImGui::MenuItem(element.visible ? "Hide" : "Show")) {
        element.visible = !element.visible;
        scene.syncVisibilityCache();
    }
    if (ImGui::MenuItem("Show All")) showAllIfc(scene);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy GUID")) ImGui::SetClipboardText(element.guid.c_str());
    ImGui::EndPopup();
}

void Editor::drawSpatialNode(int instanceIndex, IfcScene& scene, const std::string& spatialGuid)
{
    auto it = scene.spatial.find(spatialGuid);
    if (it == scene.spatial.end())
        return;
    IfcSpatialNode& node = it->second;

    // Empty branches (no elements anywhere below the next level) are hidden.
    const bool hasChildren = std::any_of(node.childSpatialGuids.begin(), node.childSpatialGuids.end(),
        [&](const std::string& childGuid) {
            auto child = scene.spatial.find(childGuid);
            return child != scene.spatial.end() &&
                (!child->second.elementGuids.empty() || !child->second.childSpatialGuids.empty());
        });
    if (node.elementGuids.empty() && !hasChildren)
        return;

    ImGui::PushID(node.guid.c_str());
    bool branchVisible = node.visible;
    if (ImGui::Checkbox("##sv", &branchVisible)) {
        scene.setVisibilityRecursive(node.guid, branchVisible);
        scene.syncVisibilityCache();
    }
    ImGui::SameLine();

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick |
        ImGuiTreeNodeFlags_SpanAvailWidth;
    if (node.selected) flags |= ImGuiTreeNodeFlags_Selected;
    if (m_openSpatialGuids.count(node.guid))
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);

    const char* label = node.name.empty() ? node.type.c_str() : node.name.c_str();
    const bool open = ImGui::TreeNodeEx("##spatial", flags, "%s", label);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen())
        selectIfcSpatial(instanceIndex, scene, node.guid);
    if (node.selected && m_ifcSelectionKind == IfcSelectionKind::Spatial && m_openSpatialGuids.count(node.guid)) {
        ImGui::SetScrollHereY(0.5f);
        m_openSpatialGuids.clear();
    }
    drawSpatialContextMenu(instanceIndex, scene, node);
    ImGui::SameLine();
    ImGui::TextDisabled("%s  (%zu)", node.type.c_str(), node.elementGuids.size() + node.childSpatialGuids.size());

    if (open) {
        for (const auto& childGuid : node.childSpatialGuids)
            drawSpatialNode(instanceIndex, scene, childGuid);
        drawSpatialElements(instanceIndex, scene, node);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void Editor::drawSpatialContextMenu(int instanceIndex, IfcScene& scene, IfcSpatialNode& node)
{
    if (!ImGui::BeginPopupContextItem("SpatialContextMenu"))
        return;
    if (ImGui::MenuItem("Select")) selectIfcSpatial(instanceIndex, scene, node.guid);
    ImGui::Separator();
    if (ImGui::MenuItem("Show Branch")) {
        scene.setVisibilityRecursive(node.guid, true);
        scene.syncVisibilityCache();
    }
    if (ImGui::MenuItem("Hide Branch")) {
        scene.setVisibilityRecursive(node.guid, false);
        scene.syncVisibilityCache();
    }
    if (ImGui::MenuItem("Show Only This Branch")) showOnlyIfcBranch(scene, node.guid);
    ImGui::Separator();
    if (ImGui::MenuItem("Copy GUID")) ImGui::SetClipboardText(node.guid.c_str());
    ImGui::EndPopup();
}

void Editor::drawSpatialElements(int instanceIndex, IfcScene& scene, IfcSpatialNode& node)
{
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(node.elementGuids.size()));
    // The clipper would skip the picked element's row, and with it the scroll-to-selection.
    if (!m_openSpatialGuids.empty() && m_ifcSelectionKind == IfcSelectionKind::Element) {
        auto selected = std::find(node.elementGuids.begin(), node.elementGuids.end(), m_selectedIfcGuid);
        if (selected != node.elementGuids.end())
            clipper.IncludeItemByIndex(static_cast<int>(selected - node.elementGuids.begin()));
    }
    while (clipper.Step()) {
        for (int e = clipper.DisplayStart; e < clipper.DisplayEnd; ++e) {
            auto it = scene.elements.find(node.elementGuids[e]);
            if (it != scene.elements.end())
                drawElementRow(instanceIndex, scene, it->second);
        }
    }
}
