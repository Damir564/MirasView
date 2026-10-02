#pragma once
#include <string>
#include "engine/VulkanContext.h"

enum class LaunchMode {
    Editor,
    Game,
};

struct AppOptions {
    LaunchMode mode = LaunchMode::Editor;
    bool validation = false;
    // Backends before this one are skipped; see VulkanBackend.
    VulkanBackend firstVulkanBackend = VulkanBackend::Native;
    int exitAfterFrames = -1; // -1 = run until the window is closed
    std::string scenePath;
    std::string settingsPath; // empty = settings.json next to the executable
    bool showHelp = false;
};

// Unknown or malformed arguments log a warning and are ignored.
AppOptions parseAppOptions(int argc, char** argv);
// Prints the usage to the console, or shows it in a message box in release builds (they have no console).
void showUsage(const char* executableName);
