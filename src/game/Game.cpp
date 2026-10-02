#include "Game.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <limits>
#include "imgui.h"
#include "app/SettingsUi.h"
#include "engine/GraphicsSettings.h"
#include "engine/ModelManager.h"
#include "engine/Renderer.h"
#include "engine/SceneManager.h"
#include "engine/Log.h"

namespace {
constexpr float kButtonWidth = 320.0f;
constexpr float kButtonHeight = 56.0f;
constexpr float kSprintMultiplier = 3.0f;
// Scene loads that make no progress this long (e.g. every model file missing) are reported as failed.
constexpr float kLoadingStallSeconds = 1.0f;
constexpr ImGuiWindowFlags kScreenFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoBringToFrontOnFocus;
}

Game::Game(const EngineContext& engine, std::string levelPath)
    : m_window(engine.window)
    , m_renderer(engine.renderer)
    , m_models(engine.models)
    , m_scenes(engine.scenes)
    , m_settings(engine.settings)
    , m_levelPath(std::move(levelPath))
{
    m_camera.position = glm::vec3(0.0f, 2.0f, 5.0f);
    SDL_SetWindowTitle(m_window, "MirasEngine");
    SDL_SetWindowRelativeMouseMode(m_window, false);
}

Game::~Game()
{
    if (m_window)
        SDL_SetWindowRelativeMouseMode(m_window, false);
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

void Game::setState(State state)
{
    const bool wasPlaying = m_state == State::Playing;
    m_state = state;
    const bool playing = state == State::Playing;
    SDL_SetWindowRelativeMouseMode(m_window, playing);
    if (playing && !wasPlaying) {
        // ImGui stops receiving events while playing; release what it thinks is held so the
        // menu does not come back with a stuck button (the click that pressed Resume/Play).
        ImGuiIO& io = ImGui::GetIO();
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        io.ClearInputKeys();
    }
}

void Game::startLoading()
{
    m_error.clear();
    if (!std::filesystem::exists(m_levelPath)) {
        failLoading(m_levelPath + " not found");
        return;
    }
    const SceneManager::OpenResult result = m_scenes.open(m_levelPath);
    if (!result.ok) {
        failLoading("Failed to open " + m_levelPath);
        return;
    }
    for (const std::string& missing : result.missingFiles)
        LOG_ERROR("[GAME] Model file not found: " << missing << "\n");
    m_loadingStallTime = 0.0f;
    setState(State::Loading);
}

void Game::updateLoading(float dt)
{
    if (!m_scenes.isLoading()) {
        if (m_models.getInstances().empty()) {
            failLoading("Failed to load " + m_levelPath + " (the scene is empty)");
            return;
        }
        frameSceneBounds();
        setState(State::Playing);
        return;
    }

    const auto& tasks = m_models.getLoadingTasks();
    const bool anyActive = std::any_of(tasks.begin(), tasks.end(),
        [](const LoadingTask& task) { return task.state != LoadingState::Failed; });
    m_loadingStallTime = anyActive ? 0.0f : m_loadingStallTime + dt;
    if (m_loadingStallTime > kLoadingStallSeconds)
        failLoading("Failed to load " + m_levelPath);
}

void Game::failLoading(const std::string& message)
{
    LOG_ERROR("[GAME] " << message << "\n");
    m_scenes.clear();
    m_error = message;
    setState(State::MainMenu);
}

void Game::returnToMainMenu()
{
    m_scenes.clear();
    m_scenes.setCurrentPath({});
    m_error.clear();
    setState(State::MainMenu);
}

// Puts the camera where the whole loaded scene is in view, looking at its center from above and in front.
void Game::frameSceneBounds()
{
    glm::vec3 boundsMin(std::numeric_limits<float>::max());
    glm::vec3 boundsMax(std::numeric_limits<float>::lowest());
    bool any = false;
    for (const ModelInstance& instance : m_models.getInstances()) {
        const GPUModel* model = m_models.getModel(instance.modelIndex);
        if (!instance.visible || !model || !model->isValid())
            continue;
        const glm::mat4 transform = instance.getTransformMatrix();
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 local(corner & 1 ? model->boundsMax.x : model->boundsMin.x,
                corner & 2 ? model->boundsMax.y : model->boundsMin.y,
                corner & 4 ? model->boundsMax.z : model->boundsMin.z);
            const glm::vec3 world = glm::vec3(transform * glm::vec4(local, 1.0f));
            boundsMin = glm::min(boundsMin, world);
            boundsMax = glm::max(boundsMax, world);
            any = true;
        }
    }
    if (!any) {
        m_camera.position = glm::vec3(0.0f, 2.0f, 5.0f);
        m_camera.yaw = -90.0f;
        m_camera.pitch = 0.0f;
        return;
    }
    const glm::vec3 center = (boundsMin + boundsMax) * 0.5f;
    const float radius = std::max(glm::length(boundsMax - boundsMin) * 0.5f, 0.5f);
    const glm::vec3 viewDir = glm::normalize(glm::vec3(0.0f, -0.35f, -1.0f));
    // 60 degree vertical FOV: a sphere of radius r fits at distance r / sin(30deg) = 2r.
    const float distance = std::max(radius * 2.2f, 2.0f);
    m_camera.position = center - viewDir * distance;
    m_camera.yaw = glm::degrees(std::atan2(viewDir.z, viewDir.x));
    m_camera.pitch = glm::degrees(std::asin(viewDir.y));
    m_camera.speed = std::clamp(radius * 0.5f, 5.0f, 200.0f);
}

// ---------------------------------------------------------------------------------------------
// Input and frame
// ---------------------------------------------------------------------------------------------

void Game::onEvent(const SDL_Event& event)
{
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_ESCAPE) {
        if (m_state == State::Playing) setState(State::Paused);
        else if (m_state == State::Paused) setState(State::Playing);
        else if (m_state == State::Settings) setState(m_settingsReturn);
        return;
    }
    if (m_state != State::Playing)
        return;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        setState(State::Paused);
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        m_camera.yaw += event.motion.xrel * m_camera.sensitivity;
        m_camera.pitch = glm::clamp(m_camera.pitch - event.motion.yrel * m_camera.sensitivity, -89.0f, 89.0f);
    }
}

void Game::update(float dt)
{
    if (m_state == State::Loading)
        updateLoading(dt);
    else if (m_state == State::Playing)
        moveCamera(dt);
}

void Game::moveCamera(float dt)
{
    const bool* keys = SDL_GetKeyboardState(nullptr);
    const glm::vec3 front = getFront(m_camera);
    const glm::vec3 right = glm::normalize(glm::cross(front, glm::vec3(0, 1, 0)));
    const bool sprint = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
    const float step = m_camera.speed * dt * (sprint ? kSprintMultiplier : 1.0f);
    glm::vec3 move(0.0f);
    if (keys[SDL_SCANCODE_W]) move += front;
    if (keys[SDL_SCANCODE_S]) move -= front;
    if (keys[SDL_SCANCODE_D]) move += right;
    if (keys[SDL_SCANCODE_A]) move -= right;
    if (keys[SDL_SCANCODE_SPACE]) move.y += 1.0f;
    if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) move.y -= 1.0f;
    if (glm::length(move) > 0.0f)
        m_camera.position += glm::normalize(move) * step;
}

void Game::fillFrame(FrameInput& frame)
{
    frame.models = &m_models;
    frame.view = getView(m_camera);
    frame.proj = getProjection(frame.windowWidth, frame.windowHeight, kCameraNearPlane, m_settings.viewDistance);
    frame.cameraPosition = m_camera.position;
    frame.viewport = { 0.0f, 0.0f, frame.windowWidth, frame.windowHeight };
    frame.highlight = {};
    frame.showPath = false;
    frame.showGrid = false;
}

// ---------------------------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------------------------

void Game::drawUi()
{
    switch (m_state) {
    case State::MainMenu: drawMainMenu(); break;
    case State::Loading:  drawLoadingScreen(); break;
    case State::Paused:   drawPauseMenu(); break;
    case State::Settings: drawSettingsScreen(); break;
    case State::Playing:  break;
    }
}

bool Game::beginScreen(const char* id, float backgroundAlpha)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.065f, 0.08f, backgroundAlpha));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const bool open = ImGui::Begin(id, nullptr, kScreenFlags);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    return open;
}

bool Game::menuButton(const char* label)
{
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - kButtonWidth) * 0.5f);
    const bool pressed = ImGui::Button(label, ImVec2(kButtonWidth, kButtonHeight));
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    return pressed;
}

namespace {

void centeredText(const char* text, float scale, const ImVec4& color)
{
    ImGui::SetWindowFontScale(scale);
    const float width = ImGui::CalcTextSize(text).x;
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
    ImGui::TextColored(color, "%s", text);
    ImGui::SetWindowFontScale(1.0f);
}

// Vertically centers a block of the given height in the current window.
void centerBlock(float height)
{
    ImGui::SetCursorPosY(std::max((ImGui::GetWindowHeight() - height) * 0.5f, 20.0f));
}

} // namespace

void Game::drawMainMenu()
{
    if (beginScreen("##MainMenu", 1.0f)) {
        const float blockHeight = 3.0f * (kButtonHeight + 14.0f) + 160.0f;
        centerBlock(blockHeight);
        centeredText("MirasEngine", 3.0f, ImVec4(0.95f, 0.95f, 0.97f, 1.0f));
        ImGui::Dummy(ImVec2(0.0f, 40.0f));

        ImGui::SetWindowFontScale(1.4f);
        if (menuButton("Play")) startLoading();
        if (menuButton("Settings")) {
            m_settingsReturn = State::MainMenu;
            setState(State::Settings);
        }
        if (menuButton("Exit")) m_quitRequested = true;
        ImGui::SetWindowFontScale(1.0f);

        if (!m_error.empty()) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            centeredText(m_error.c_str(), 1.2f, ImVec4(1.0f, 0.4f, 0.35f, 1.0f));
        }
    }
    ImGui::End();
}

void Game::drawLoadingScreen()
{
    if (beginScreen("##Loading", 1.0f)) {
        centerBlock(60.0f);
        static constexpr char kSpinner[] = "|/-\\";
        const char spin = kSpinner[static_cast<int>(ImGui::GetTime() * 8.0) % 4];
        const std::string text = std::string("Loading ") + m_levelPath + "...  " + spin;
        centeredText(text.c_str(), 1.6f, ImVec4(0.85f, 0.85f, 0.9f, 1.0f));
    }
    ImGui::End();
}

void Game::drawPauseMenu()
{
    if (beginScreen("##Paused", 0.6f)) {
        centerBlock(4.0f * (kButtonHeight + 14.0f) + 110.0f);
        centeredText("Paused", 2.2f, ImVec4(0.95f, 0.95f, 0.97f, 1.0f));
        ImGui::Dummy(ImVec2(0.0f, 30.0f));
        ImGui::SetWindowFontScale(1.4f);
        if (menuButton("Resume")) setState(State::Playing);
        if (menuButton("Settings")) {
            m_settingsReturn = State::Paused;
            setState(State::Settings);
        }
        if (menuButton("Main Menu")) returnToMainMenu();
        if (menuButton("Exit")) m_quitRequested = true;
        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
}

void Game::drawSettingsScreen()
{
    // Over the paused scene the settings stay translucent so their effect is visible.
    const float alpha = m_settingsReturn == State::Paused ? 0.75f : 1.0f;
    if (beginScreen("##Settings", alpha)) {
        const float panelWidth = 480.0f;
        const float panelHeight = std::clamp(ImGui::GetWindowHeight() - 260.0f, 150.0f, 460.0f);
        centerBlock(panelHeight + 120.0f);
        centeredText("Settings", 2.2f, ImVec4(0.95f, 0.95f, 0.97f, 1.0f));
        ImGui::Dummy(ImVec2(0.0f, 20.0f));
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - panelWidth) * 0.5f);
        if (ImGui::BeginChild("##settingsPanel", ImVec2(panelWidth, panelHeight), ImGuiChildFlags_Borders))
            drawGraphicsSettings(m_settings, m_renderer.capabilities());
        ImGui::EndChild();
        ImGui::Dummy(ImVec2(0.0f, 16.0f));
        ImGui::SetWindowFontScale(1.4f);
        if (menuButton("Back")) setState(m_settingsReturn);
        ImGui::SetWindowFontScale(1.0f);
    }
    ImGui::End();
}
