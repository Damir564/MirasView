#pragma once
#include <memory>
#include "AppOptions.h"
#include "engine/GraphicsSettings.h"
#include "engine/Renderer.h"
#include "engine/VulkanContext.h"

struct SDL_Window;
class AppMode;
class ModelManager;
class SceneManager;

// Owns the window, the engine objects and the ImGui context, and runs the active AppMode.
class Application {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    // Returns the process exit code.
    int run(const AppOptions& options);

private:
    bool init(const AppOptions& options);
    bool initWindow();
    void initImGui();
    bool createModelManager();
    void createMode(const AppOptions& options);
    void shutdown();

    int mainLoop(int exitAfterFrames);
    void pollEvents();
    // False while there is nothing to render into (minimized, zero-sized, swapchain not rebuildable).
    bool readyToRender();
    ImDrawData* buildUi();
    // Pushes settings the mode changed this frame to the renderer and settings.json.
    void applyChangedSettings();
    // Sleeps until the frame that started at frameStartNs has lasted 1 / maxFps.
    void limitFrameRate(uint64_t frameStartNs) const;

    SDL_Window* m_window = nullptr;
    bool m_sdlInitialized = false;
    bool m_imguiInitialized = false;
    bool m_quit = false;

    std::string m_settingsPath = kGraphicsSettingsPath;
    GraphicsSettings m_settings;
    GraphicsSettings m_appliedSettings;

    // Destruction order matters: the mode uses everything, SceneManager uses ModelManager, and
    // ModelManager allocates from the renderer's pools; all of them need the Vulkan device.
    VulkanContext m_vulkan;
    Renderer m_renderer;
    std::unique_ptr<ModelManager> m_models;
    std::unique_ptr<SceneManager> m_scenes;
    std::unique_ptr<AppMode> m_mode;
};
