#include "SceneManager.h"
#include "ModelLoader.h"
#include "ModelManager.h"
#include <filesystem>
#include "Log.h"

SceneManager::SceneManager(ModelManager& models, vk::Device device)
    : m_models(models), m_device(device)
{
}

SceneManager::OpenResult SceneManager::open(const std::string& path)
{
    OpenResult result;
    SceneSerializer::LoadedScene loaded = SceneSerializer::Load(path);
    if (!loaded.valid)
        return result;

    clear();
    for (const auto& model : loaded.models) {
        if (!isBuiltinModelPath(model.path) && !std::filesystem::exists(model.path)) {
            result.missingFiles.push_back(model.path);
            continue;
        }
        m_models.loadModelAsync(model.path, model.name);
        ++result.queuedModels;
    }

    m_pendingScene = std::move(loaded);
    m_pendingLoad = true;
    m_currentPath = path;
    result.ok = true;
    return result;
}

bool SceneManager::save(const std::string& path)
{
    return SceneSerializer::Save(path, m_models);
}

void SceneManager::clear()
{
    (void)m_device.waitIdle();
    auto& instances = m_models.getInstances();
    while (!instances.empty())
        m_models.removeInstance(instances.size() - 1);
    while (!m_models.getModels().empty())
        m_models.unloadModel(m_models.getModels().size() - 1);
    m_pendingLoad = false;
}

void SceneManager::update()
{
    if (m_pendingLoad)
        instantiatePendingScene();
}

void SceneManager::instantiatePendingScene()
{
    const auto& loadedModels = m_models.getModels();
    const bool allDone = m_models.getLoadingTasks().empty() && !loadedModels.empty();
    if (!allDone || loadedModels.size() < m_pendingScene.models.size())
        return;

    // Scene files index models by their own order; map that onto the manager's slots by source path.
    std::vector<int> fileToManager(m_pendingScene.models.size(), -1);
    for (size_t fi = 0; fi < m_pendingScene.models.size(); ++fi) {
        for (size_t mi = 0; mi < loadedModels.size(); ++mi) {
            if (loadedModels[mi] && loadedModels[mi]->isValid() &&
                loadedModels[mi]->sourcePath == m_pendingScene.models[fi].path) {
                fileToManager[fi] = static_cast<int>(mi);
                break;
            }
        }
    }

    m_models.reserveInstances(m_pendingScene.instances.size());
    for (const auto& inst : m_pendingScene.instances) {
        if (inst.fileModelIndex >= fileToManager.size()) continue;
        const int managerIdx = fileToManager[inst.fileModelIndex];
        if (managerIdx < 0) continue;

        const size_t newIdx = m_models.createInstance(static_cast<size_t>(managerIdx), inst.position);
        auto& newInst = m_models.getInstances()[newIdx];
        newInst.name = inst.name;
        newInst.rotation = inst.rotation;
        newInst.scale = inst.scale;
        newInst.visible = inst.visible;
        newInst.color = inst.color;
        newInst.materials = inst.materials;
    }

    m_pendingLoad = false;
    LOG_INFO("[SCENE] All instances created\n");
}
