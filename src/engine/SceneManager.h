#pragma once
#include <vulkan/vulkan.hpp>
#include <cstddef>
#include <string>
#include <vector>
#include "SceneSerializer.h"

class ModelManager;

// Opens, saves and clears .scn scenes on top of ModelManager. Opening is asynchronous: models load
// in the background and update() creates the scene's instances once they are all ready.
class SceneManager {
public:
    struct OpenResult {
        bool ok = false;
        size_t queuedModels = 0;
        std::vector<std::string> missingFiles;
    };

    SceneManager(ModelManager& models, vk::Device device);

    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;

    // Replaces the current scene; on failure (unreadable file) the current scene is left untouched.
    OpenResult open(const std::string& path);
    bool save(const std::string& path);
    // Removes every instance and model. Does not change currentPath().
    void clear();
    // Call once per frame: advances async model loads and instantiates a pending scene when ready.
    void update();

    bool isLoading() const { return m_pendingLoad; }
    const std::string& currentPath() const { return m_currentPath; }
    void setCurrentPath(std::string path) { m_currentPath = std::move(path); }

private:
    void instantiatePendingScene();

    ModelManager& m_models;
    vk::Device m_device;
    std::string m_currentPath;
    bool m_pendingLoad = false;
    SceneSerializer::LoadedScene m_pendingScene;
};
