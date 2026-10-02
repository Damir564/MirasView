#include "Application.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include <exception>
#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_vulkan.h"
#include "AppMode.h"
#include "editor/Editor.h"
#include "editor/EditorStyle.h"
#include "game/Game.h"
#include "engine/ModelManager.h"
#include "engine/SceneManager.h"
#include "engine/Log.h"

namespace {
constexpr int kInitialWindowWidth = 1600;
constexpr int kInitialWindowHeight = 900;
}

Application::Application() = default;

Application::~Application()
{
    shutdown();
}

int Application::run(const AppOptions& options)
{
    if (!init(options)) {
        shutdown();
#ifdef NDEBUG
        // Release builds have no console showing the logged reason. Automated runs must not block on a dialog.
        if (options.exitAfterFrames < 0)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MirasEngine",
                "Failed to start: neither the graphics driver nor the bundled software renderer could initialize Vulkan.\n"
                "Make sure the \"vulkan\" folder next to engine.exe is complete, or update the graphics driver.", nullptr);
#endif
        return -1;
    }
    const int exitCode = mainLoop(options.exitAfterFrames);
    shutdown();
    return exitCode;
}

bool Application::init(const AppOptions& options)
{
    if (!initWindow())
        return false;
    if (!m_vulkan.init(m_window, options.validation, options.firstVulkanBackend))
        return false;
    // The renderer initializes the ImGui Vulkan backend, so the ImGui context must exist first.
    initImGui();
    if (!options.settingsPath.empty())
        m_settingsPath = options.settingsPath;
    m_settings = loadGraphicsSettings(m_settingsPath);
    if (!m_renderer.init(m_vulkan, m_window, m_settings))
        return false;
    m_appliedSettings = m_settings;
    if (!createModelManager())
        return false;
    m_scenes = std::make_unique<SceneManager>(*m_models, m_vulkan.device());
    createMode(options);
    return true;
}

bool Application::initWindow()
{
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        LOG_ERROR("SDL_Init failed: " << SDL_GetError() << "\n");
        return false;
    }
    m_sdlInitialized = true;
    // Before the window: SDL uses whichever Vulkan loader is already loaded instead of loading its own.
    if (!m_vulkan.loadLibrary())
        return false;

    // The active mode sets the real title.
    m_window = SDL_CreateWindow("MirasEngine", kInitialWindowWidth, kInitialWindowHeight,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!m_window) {
        LOG_ERROR("SDL_CreateWindow failed: " << SDL_GetError() << "\n");
        return false;
    }
    return true;
}

void Application::initImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    EditorStyle::loadFonts(io);
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    EditorStyle::apply();
    ImGui_ImplSDL3_InitForVulkan(m_window);
    m_imguiInitialized = true;
}

bool Application::createModelManager()
{
    try {
        m_models = std::make_unique<ModelManager>(
            m_vulkan.allocator(), m_vulkan.device(), m_renderer.commandPool(), m_vulkan.graphicsQueue(),
            m_renderer.descriptorPool(), m_renderer.textureSetLayout(), m_renderer.textureSampler());
        LOG_INFO("ModelManager created successfully\n");
        return true;
    }
    catch (const std::exception& e) {
        LOG_ERROR("Failed to create ModelManager: " << e.what() << "\n");
        return false;
    }
}

void Application::createMode(const AppOptions& options)
{
    EngineContext context{ m_window, m_vulkan, m_renderer, *m_models, *m_scenes, m_settings };
    if (options.mode == LaunchMode::Game) {
        // --scene replaces the default level.
        m_mode = options.scenePath.empty() ? std::make_unique<Game>(context)
                                           : std::make_unique<Game>(context, options.scenePath);
        return;
    }
    auto editor = std::make_unique<Editor>(context);
    if (!options.scenePath.empty())
        editor->openScene(options.scenePath);
    m_mode = std::move(editor);
}

void Application::shutdown()
{
    m_mode.reset();
    m_scenes.reset();
    // ModelManager frees descriptor sets and command buffers from the renderer's pools.
    m_models.reset();
    m_renderer.shutdown();
    if (m_imguiInitialized) {
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        m_imguiInitialized = false;
    }
    m_vulkan.shutdown();
    if (m_window) {
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
    }
    if (m_sdlInitialized) {
        SDL_Quit();
        m_sdlInitialized = false;
    }
}

int Application::mainLoop(int exitAfterFrames)
{
    uint64_t framesRendered = 0;
    float time = 0.0f;
    uint64_t lastTicks = SDL_GetTicks();

    while (!m_quit && !m_mode->quitRequested()) {
        const uint64_t frameStartNs = SDL_GetTicksNS();
        const uint64_t ticks = SDL_GetTicks();
        const float dt = (ticks - lastTicks) / 1000.0f;
        lastTicks = ticks;

        pollEvents();
        if (!readyToRender()) {
            SDL_Delay(10);
            continue;
        }

        m_models->update();
        m_scenes->update();
        m_mode->update(dt);
        ImDrawData* drawData = buildUi();
        m_mode->lateUpdate(dt);
        applyChangedSettings();

        // Mouse coordinates and ImGui use window coordinates; rendering uses framebuffer pixels.
        int windowWidth = 0, windowHeight = 0;
        SDL_GetWindowSize(m_window, &windowWidth, &windowHeight);
        FrameInput frame;
        frame.windowWidth = static_cast<float>(std::max(windowWidth, 1));
        frame.windowHeight = static_cast<float>(std::max(windowHeight, 1));
        frame.imgui = drawData;
        frame.time = time;
        m_mode->fillFrame(frame);

        const Renderer::FrameStatus status = m_renderer.renderFrame(frame);
        if (status == Renderer::FrameStatus::Failed)
            return -1;
        if (status == Renderer::FrameStatus::Skipped)
            continue;
        limitFrameRate(frameStartNs);

        ++framesRendered;
        if (exitAfterFrames > 0 && framesRendered >= static_cast<uint64_t>(exitAfterFrames))
            m_quit = true;
        time += 0.001f;
    }
    return 0;
}

void Application::applyChangedSettings()
{
    if (m_settings == m_appliedSettings)
        return;
    m_settings = sanitizeGraphicsSettings(m_settings);
    m_renderer.applySettings(m_settings);
    saveGraphicsSettings(m_settings, m_settingsPath);
    m_appliedSettings = m_settings;
}

void Application::limitFrameRate(uint64_t frameStartNs) const
{
    if (m_settings.maxFps <= 0)
        return;
    const uint64_t targetNs = 1'000'000'000ull / static_cast<uint64_t>(m_settings.maxFps);
    const uint64_t elapsedNs = SDL_GetTicksNS() - frameStartNs;
    // SDL_DelayPrecise sleeps most of the time and spins only for the last bit, so it stays accurate.
    if (elapsedNs < targetNs)
        SDL_DelayPrecise(targetNs - elapsedNs);
}

void Application::pollEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        // Checked per event: the mode may hide its UI in response to an earlier event.
        if (m_mode->uiVisible())
            ImGui_ImplSDL3_ProcessEvent(&event);

        if (event.type == SDL_EVENT_QUIT)
            m_quit = true;
        if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED || event.type == SDL_EVENT_WINDOW_RESIZED)
            m_renderer.requestSwapchainRebuild();

        m_mode->onEvent(event);
    }
}

bool Application::readyToRender()
{
    // While minimized the drawable size is 0x0 and no swapchain can exist; idle until restored.
    int width = 0, height = 0;
    SDL_GetWindowSize(m_window, &width, &height);
    if ((SDL_GetWindowFlags(m_window) & SDL_WINDOW_MINIMIZED) || width <= 0 || height <= 0)
        return false;
    return m_renderer.prepareSwapchain();
}

ImDrawData* Application::buildUi()
{
    if (!m_mode->uiVisible())
        return nullptr;
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    m_mode->drawUi();
    ImGui::Render();
    return ImGui::GetDrawData();
}
