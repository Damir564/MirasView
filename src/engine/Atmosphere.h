#pragma once
#include <glm/glm.hpp>

// CPU half of the sky model in shaders/sky.glsl: a single-scattering Rayleigh + Mie atmosphere with an
// ozone layer, seen from a little above the ground. The constants in Atmosphere.cpp must match the shader.

// The sun spans about half a degree.
inline constexpr float kSunAngularRadius = 0.00465f;

// Direction the sunlight travels, from compass angles in degrees (azimuth 0 = -Z, 90 = +X).
glm::vec3 sunDirectionFromAngles(float azimuthDegrees, float elevationDegrees);
// Fraction of sunlight that reaches the observer through the atmosphere; zero once the sun has set.
glm::vec3 sunTransmittance(const glm::vec3& toSun, float haze);
glm::vec3 srgbToLinear(const glm::vec3& color);
