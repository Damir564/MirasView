#include "Atmosphere.h"
#include <algorithm>
#include <cmath>

namespace {

// Keep in sync with shaders/sky.glsl.
constexpr float kPlanetRadius = 6360e3f;
constexpr float kAtmosphereRadius = 6460e3f;
constexpr float kObserverAltitude = 200.0f;
const glm::vec3 kRayleighScattering(5.802e-6f, 13.558e-6f, 33.1e-6f);
constexpr float kRayleighHeight = 8000.0f;
constexpr float kMieExtinction = 4.44e-6f;
constexpr float kMieHeight = 1200.0f;
const glm::vec3 kOzoneAbsorption(0.650e-6f, 1.881e-6f, 0.085e-6f);

glm::vec3 extinctionAt(float altitude, float haze)
{
    const float rayleigh = std::exp(-altitude / kRayleighHeight);
    const float mie = std::exp(-altitude / kMieHeight);
    const float ozone = std::max(0.0f, 1.0f - std::abs(altitude - 25e3f) / 15e3f);
    return kRayleighScattering * rayleigh + glm::vec3(kMieExtinction * haze * mie) + kOzoneAbsorption * ozone;
}

// Discriminant of a ray starting at radius r with cosine mu to the zenith against a sphere of radius R,
// written so the nearly equal radii do not cancel.
float sphereDiscriminant(float r, float mu, float R)
{
    return r * r * mu * mu + (R - r) * (R + r);
}

} // namespace

glm::vec3 sunDirectionFromAngles(float azimuthDegrees, float elevationDegrees)
{
    const float azimuth = glm::radians(azimuthDegrees);
    const float elevation = glm::radians(elevationDegrees);
    const glm::vec3 toSun(std::sin(azimuth) * std::cos(elevation), std::sin(elevation),
        -std::cos(azimuth) * std::cos(elevation));
    return -toSun;
}

glm::vec3 sunTransmittance(const glm::vec3& toSun, float haze)
{
    const float r = kPlanetRadius + kObserverAltitude;
    const float mu = toSun.y;
    const float groundDisc = sphereDiscriminant(r, mu, kPlanetRadius);
    if (mu < 0.0f && groundDisc >= 0.0f)
        return glm::vec3(0.0f);

    const float length = -r * mu + std::sqrt(std::max(sphereDiscriminant(r, mu, kAtmosphereRadius), 0.0f));
    constexpr int kSteps = 64;
    glm::vec3 opticalDepth(0.0f);
    for (int i = 0; i < kSteps; ++i) {
        // Quadratic spacing puts most samples in the dense air near the ground.
        const float s0 = static_cast<float>(i) / kSteps;
        const float s1 = static_cast<float>(i + 1) / kSteps;
        const float t0 = length * s0 * s0;
        const float t1 = length * s1 * s1;
        const float t = 0.5f * (t0 + t1);
        const float altitude = std::sqrt(r * r + t * t + 2.0f * r * mu * t) - kPlanetRadius;
        opticalDepth += extinctionAt(altitude, haze) * (t1 - t0);
    }
    return glm::exp(-opticalDepth);
}

glm::vec3 srgbToLinear(const glm::vec3& color)
{
    auto channel = [](float c) {
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    return glm::vec3(channel(color.r), channel(color.g), channel(color.b));
}
