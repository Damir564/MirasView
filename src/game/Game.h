#pragma once
#include <string>
#include "app/AppMode.h"
#include "engine/Camera.h"

class Renderer;
class ModelManager;
class SceneManager;
struct GraphicsSettings;

// Player mode: main menu, loads the level scene and lets the player fly around it.
class Game final : public AppMode {
public:
    explicit Game(const EngineContext& engine, std::string levelPath = "level1.scn");
    ~Game() override;

    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    void onEvent(const SDL_Event& event) override;
    void update(float dt) override;
    bool uiVisible() const override { return m_state != State::Playing; }
    void drawUi() override;
    void fillFrame(FrameInput& frame) override;
    bool quitRequested() const override { return m_quitRequested; }

private:
    enum class State {
        MainMenu,
        Loading,
        Playing,
        Paused,
        Settings,
    };

    void setState(State state);
    void startLoading();
    void updateLoading(float dt);
    void failLoading(const std::string& message);
    void frameSceneBounds();
    void moveCamera(float dt);
    void returnToMainMenu();

    void drawMainMenu();
    void drawLoadingScreen();
    void drawPauseMenu();
    void drawSettingsScreen();
    // Full-window ImGui window for a menu screen; returns the result of Begin().
    bool beginScreen(const char* id, float backgroundAlpha);
    bool menuButton(const char* label);

    SDL_Window* m_window;
    Renderer& m_renderer;
    ModelManager& m_models;
    SceneManager& m_scenes;
    GraphicsSettings& m_settings;
    std::string m_levelPath;

    State m_state = State::MainMenu;
    State m_settingsReturn = State::MainMenu;
    std::string m_error;
    float m_loadingStallTime = 0.0f;
    bool m_quitRequested = false;

    Camera m_camera;
};
