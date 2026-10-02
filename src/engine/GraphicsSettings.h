#pragma once
#include <glm/glm.hpp>
#include <string>

enum class BackgroundMode {
    SolidColor, // flat color behind the scene; lighting still comes from the sky model
    Realistic,  // physically based sky with the sun disk
};

enum class Tonemapper {
    Aces,
    AgX,
    Neutral,  // Khronos PBR Neutral: keeps material colors close to their base color
    Reinhard,
};

struct GraphicsSettings {
    // Display
    bool vsync = true;
    int maxFps = 0;            // 0 = unlimited
    int msaaSamples = 4;       // 1, 2, 4 or 8; clamped to what the device supports
    bool fxaa = false;         // post-process anti-aliasing; also smooths edges MSAA misses (alpha test, shading)

    // Shadows
    bool shadows = true;
    int shadowMapSize = 2048;  // per cascade: 1024, 2048 or 4096
    int shadowCascades = 4;    // 1..4; more cascades keep near shadows sharp over a long distance
    float shadowDistance = 150.0f;
    bool softShadows = true;   // penumbrae widen with the distance to the caster (PCSS)
    bool contactShadows = true; // screen-space shadows for small details the shadow map is too coarse for

    // Lighting and post-processing
    int ambientOcclusion = 2;  // 0 = off, 1 = low, 2 = medium, 3 = high
    float aoRadius = 1.0f;     // meters
    float aoIntensity = 1.0f;
    bool bloom = true;
    float bloomIntensity = 0.5f;
    float exposure = 0.0f;     // EV compensation
    Tonemapper tonemapper = Tonemapper::Aces;
    float contrast = 1.0f;
    float saturation = 1.0f;
    float vignette = 0.25f;

    // Environment
    BackgroundMode background = BackgroundMode::Realistic;
    glm::vec3 backgroundColor{ 0.24f, 0.26f, 0.29f }; // sRGB
    bool sun = true;           // off: no sun disk or direct sunlight (and no shadows); the sky stays lit
    float sunAzimuth = 135.0f; // degrees clockwise from -Z (north) towards +X (east)
    float sunElevation = 35.0f; // degrees above the horizon
    float sunIntensity = 1.0f;
    glm::vec3 sunColor{ 1.0f }; // sRGB tint; the atmosphere already reddens a low sun
    float haze = 1.0f;         // aerosol density of the atmosphere: whiter sky, softer sun
    bool fog = true;
    float fogDensity = 1.0f;   // multiplier of the base height fog density
    float viewDistance = 5000.0f; // camera far plane

    bool operator==(const GraphicsSettings&) const = default;
};

struct RenderCapabilities {
    int maxMsaaSamples = 1;
    float maxAnisotropy = 1.0f;
};

inline constexpr const char* kGraphicsSettingsPath = "settings.json";

// Clamps every field into its valid range (unknown MSAA/shadow sizes snap to the nearest valid value).
GraphicsSettings sanitizeGraphicsSettings(GraphicsSettings settings);
// A missing or unreadable file yields defaults; missing keys keep their default value.
GraphicsSettings loadGraphicsSettings(const std::string& path = kGraphicsSettingsPath);
bool saveGraphicsSettings(const GraphicsSettings& settings, const std::string& path = kGraphicsSettingsPath);
