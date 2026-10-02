#include "AppOptions.h"
#include "engine/Log.h"
#include <SDL3/SDL_messagebox.h>
#include <charconv>
#include <iostream>
#include <string>
#include <string_view>

namespace {

// Returns the argument after argv[i] (advancing i), or nullptr with a warning when it is missing.
const char* takeValue(int argc, char** argv, int& i, std::string_view flag, const char* what)
{
    if (i + 1 >= argc) {
        LOG_ERROR("Warning: " << flag << " needs " << what << ", ignoring\n");
        return nullptr;
    }
    return argv[++i];
}

void parseFrameCount(std::string_view value, AppOptions& options)
{
    int frames = 0;
    auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), frames);
    if (ec != std::errc() || end != value.data() + value.size() || frames <= 0)
        LOG_ERROR("Warning: invalid --exit-after-frames value '" << value << "', ignoring\n");
    else
        options.exitAfterFrames = frames;
}

void parseVulkanBackend(std::string_view value, AppOptions& options)
{
    if (value == "auto")
        options.firstVulkanBackend = VulkanBackend::Native;
    else if (value == "emulated")
        options.firstVulkanBackend = VulkanBackend::Emulated;
    else if (value == "software")
        options.firstVulkanBackend = VulkanBackend::Software;
    else
        LOG_ERROR("Warning: invalid --vulkan value '" << value << "', ignoring\n");
}

} // namespace

AppOptions parseAppOptions(int argc, char** argv)
{
    AppOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--editor") {
            options.mode = LaunchMode::Editor;
        }
        else if (arg == "--game") {
            options.mode = LaunchMode::Game;
        }
        else if (arg == "--validation") {
            options.validation = true;
        }
        else if (arg == "--vulkan") {
            if (const char* value = takeValue(argc, argv, i, arg, "auto, emulated or software"))
                parseVulkanBackend(value, options);
        }
        else if (arg == "--exit-after-frames") {
            if (const char* value = takeValue(argc, argv, i, arg, "a frame count"))
                parseFrameCount(value, options);
        }
        else if (arg == "--scene") {
            if (const char* value = takeValue(argc, argv, i, arg, "a scene path"))
                options.scenePath = value;
        }
        else if (arg == "--settings") {
            if (const char* value = takeValue(argc, argv, i, arg, "a settings path"))
                options.settingsPath = value;
        }
        else if (arg == "--help" || arg == "-h") {
            options.showHelp = true;
        }
        else {
            LOG_ERROR("Warning: unknown argument '" << arg << "', ignoring\n");
        }
    }
    return options;
}

void showUsage(const char* executableName)
{
    const std::string usage = std::string("Usage: ") + executableName + " [options]\n"
        "\n"
        "Options:\n"
        "  --editor                 Start the editor (default)\n"
        "  --game                   Start the game (main menu, plays level1.scn)\n"
        "  --scene <path>           Editor: open this .scn at startup; game: use it as the level\n"
        "  --settings <path>        Load and save graphics settings here instead of settings.json\n"
        "  --validation             Enable the Vulkan validation layers\n"
        "  --vulkan <mode>          auto (default): the GPU driver, then emulation layers, then the CPU\n"
        "                           emulated: force the emulation layers; software: render on the CPU\n"
        "  --exit-after-frames <N>  Quit after N rendered frames (for automated runs)\n"
        "  --help, -h               Show this help and exit\n";
#ifdef NDEBUG
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "MirasEngine", usage.c_str(), nullptr);
#else
    std::cout << usage;
#endif
}
