#include "app/AppOptions.h"
#include "app/Application.h"
#include <SDL3/SDL.h>
#include <filesystem>

int main(int argc, char** argv)
{
    // Shaders, settings and helper exes are resolved relative to the executable's folder,
    // regardless of the working directory the launcher (e.g. Visual Studio) picked.
    if (const char* basePath = SDL_GetBasePath()) {
        std::error_code ec;
        std::filesystem::current_path(std::filesystem::path(reinterpret_cast<const char8_t*>(basePath)), ec);
    }


    const AppOptions options = parseAppOptions(argc, argv);
    if (options.showHelp) {
        showUsage(argc > 0 ? argv[0] : "engine");
        return 0;
    }
    Application app;
    return app.run(options);
}
