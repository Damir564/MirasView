#include "SettingsUi.h"
#include <algorithm>
#include "imgui.h"

namespace {

// Combo over a fixed list of int values; values above maxValue are hidden.
bool comboInt(const char* label, int& value, const int* values, const char* const* names, int count, int maxValue)
{
    const char* preview = "Custom";
    for (int i = 0; i < count; ++i)
        if (values[i] == value) preview = names[i];

    bool changed = false;
    if (ImGui::BeginCombo(label, preview)) {
        for (int i = 0; i < count; ++i) {
            if (values[i] > maxValue) continue;
            const bool selected = values[i] == value;
            if (ImGui::Selectable(names[i], selected) && !selected) {
                value = values[i];
                changed = true;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// Combo over an enum whose values are 0..count-1 in the order of `names`.
template <typename Enum>
bool comboEnum(const char* label, Enum& value, const char* const* names, int count)
{
    int index = static_cast<int>(value);
    if (!ImGui::Combo(label, &index, names, count))
        return false;
    value = static_cast<Enum>(index);
    return true;
}

enum class QualityPreset { Low, Medium, High, Ultra };

// Presets only touch the performance-relevant options, not the look (sun, exposure, grading...).
void applyPreset(GraphicsSettings& s, QualityPreset preset, const RenderCapabilities& caps)
{
    switch (preset) {
    case QualityPreset::Low:
        s.msaaSamples = 1; s.fxaa = true; s.shadowMapSize = 1024; s.shadowCascades = 2;
        s.softShadows = false; s.contactShadows = false; s.ambientOcclusion = 0; s.bloom = false;
        break;
    case QualityPreset::Medium:
        s.msaaSamples = 2; s.fxaa = false; s.shadowMapSize = 2048; s.shadowCascades = 3;
        s.softShadows = false; s.contactShadows = false; s.ambientOcclusion = 1; s.bloom = true;
        break;
    case QualityPreset::High:
        s.msaaSamples = 4; s.fxaa = false; s.shadowMapSize = 2048; s.shadowCascades = 4;
        s.softShadows = true; s.contactShadows = true; s.ambientOcclusion = 2; s.bloom = true;
        break;
    case QualityPreset::Ultra:
        s.msaaSamples = 8; s.fxaa = false; s.shadowMapSize = 4096; s.shadowCascades = 4;
        s.softShadows = true; s.contactShadows = true; s.ambientOcclusion = 3; s.bloom = true;
        break;
    }
    s.msaaSamples = std::min(s.msaaSamples, caps.maxMsaaSamples);
}

constexpr ImGuiSliderFlags kClamp = ImGuiSliderFlags_AlwaysClamp;
constexpr ImGuiSliderFlags kLogClamp = ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp;

bool drawDisplay(GraphicsSettings& settings, const RenderCapabilities& caps)
{
    bool changed = ImGui::Checkbox("VSync", &settings.vsync);
    {
        static const int values[] = { 0, 30, 60, 120, 144, 165, 240 };
        static const char* const names[] = { "Unlimited", "30", "60", "120", "144", "165", "240" };
        changed |= comboInt("Max FPS", settings.maxFps, values, names, IM_ARRAYSIZE(values), 1 << 30);
    }
    {
        static const int values[] = { 1, 2, 4, 8 };
        static const char* const names[] = { "Off", "MSAA 2x", "MSAA 4x", "MSAA 8x" };
        changed |= comboInt("Anti-aliasing", settings.msaaSamples, values, names, IM_ARRAYSIZE(values), caps.maxMsaaSamples);
    }
    changed |= ImGui::Checkbox("FXAA", &settings.fxaa);
    ImGui::SetItemTooltip("Post-process anti-aliasing. Cheap, also smooths edges MSAA misses (alpha-tested\n"
        "foliage, shading), but slightly softens the image.");
    return changed;
}

bool drawShadows(GraphicsSettings& settings)
{
    // "##enabled": the section's CollapsingHeader is also labeled "Shadows" and would share the ID.
    bool changed = ImGui::Checkbox("Shadows##enabled", &settings.shadows);
    ImGui::BeginDisabled(!settings.shadows);
    {
        static const int values[] = { 1024, 2048, 4096 };
        static const char* const names[] = { "Low (1024)", "Medium (2048)", "High (4096)" };
        changed |= comboInt("Shadow quality", settings.shadowMapSize, values, names, IM_ARRAYSIZE(values), 1 << 30);
    }
    changed |= ImGui::SliderInt("Cascades", &settings.shadowCascades, 1, 4, "%d", kClamp);
    ImGui::SetItemTooltip("More cascades keep shadows sharp near the camera over a long shadow distance.");
    changed |= ImGui::SliderFloat("Shadow distance", &settings.shadowDistance, 10.0f, 1000.0f, "%.0f m", kLogClamp);
    changed |= ImGui::Checkbox("Soft shadows", &settings.softShadows);
    ImGui::SetItemTooltip("Shadows get softer further away from the object casting them, like real sunlight.");
    ImGui::EndDisabled();
    changed |= ImGui::Checkbox("Contact shadows", &settings.contactShadows);
    ImGui::SetItemTooltip("Screen-space shadows for small details the shadow map is too coarse for.");
    return changed;
}

bool drawLighting(GraphicsSettings& settings)
{
    bool changed = false;
    {
        static const int values[] = { 0, 1, 2, 3 };
        static const char* const names[] = { "Off", "Low", "Medium", "High" };
        changed |= comboInt("Ambient occlusion", settings.ambientOcclusion, values, names, IM_ARRAYSIZE(values), 3);
    }
    ImGui::BeginDisabled(settings.ambientOcclusion == 0);
    changed |= ImGui::SliderFloat("AO radius", &settings.aoRadius, 0.1f, 5.0f, "%.2f m", kLogClamp);
    changed |= ImGui::SliderFloat("AO intensity", &settings.aoIntensity, 0.0f, 4.0f, "%.2f", kClamp);
    ImGui::EndDisabled();

    changed |= ImGui::Checkbox("Bloom", &settings.bloom);
    ImGui::BeginDisabled(!settings.bloom);
    changed |= ImGui::SliderFloat("Bloom strength", &settings.bloomIntensity, 0.0f, 1.0f, "%.2f", kClamp);
    ImGui::EndDisabled();

    changed |= ImGui::SliderFloat("Exposure", &settings.exposure, -5.0f, 5.0f, "%+.1f EV", kClamp);
    {
        static const char* const names[] = { "ACES", "AgX", "Neutral", "Reinhard" };
        changed |= comboEnum("Tone mapping", settings.tonemapper, names, IM_ARRAYSIZE(names));
        ImGui::SetItemTooltip("ACES: filmic contrast. AgX: natural highlights. Neutral: keeps material colors.");
    }
    changed |= ImGui::SliderFloat("Contrast", &settings.contrast, 0.5f, 1.5f, "%.2f", kClamp);
    changed |= ImGui::SliderFloat("Saturation", &settings.saturation, 0.0f, 2.0f, "%.2f", kClamp);
    changed |= ImGui::SliderFloat("Vignette", &settings.vignette, 0.0f, 1.0f, "%.2f", kClamp);
    return changed;
}

bool drawEnvironment(GraphicsSettings& settings)
{
    bool changed = false;
    {
        static const char* const names[] = { "Solid color", "Realistic sky" };
        changed |= comboEnum("Background", settings.background, names, IM_ARRAYSIZE(names));
    }
    if (settings.background == BackgroundMode::SolidColor) {
        changed |= ImGui::ColorEdit3("Background color", &settings.backgroundColor.x);
        ImGui::SetItemTooltip("Only what is seen behind the scene; lighting still comes from the sky.");
    }

    changed |= ImGui::Checkbox("Sun", &settings.sun);
    ImGui::SetItemTooltip("Sun disk and direct sunlight. Turning it off also disables shadows.");
    ImGui::BeginDisabled(!settings.sun);
    changed |= ImGui::SliderFloat("Sun azimuth", &settings.sunAzimuth, 0.0f, 360.0f, "%.0f\xC2\xB0", kClamp);
    ImGui::SetItemTooltip("Compass direction of the sun: 0 = north (-Z), 90 = east (+X).");
    changed |= ImGui::SliderFloat("Sun elevation", &settings.sunElevation, -10.0f, 90.0f, "%.1f\xC2\xB0", kClamp);
    ImGui::SetItemTooltip("Height of the sun above the horizon. A low sun turns warm and casts long shadows.");
    changed |= ImGui::SliderFloat("Sun intensity", &settings.sunIntensity, 0.0f, 10.0f, "%.2f", kLogClamp);
    changed |= ImGui::ColorEdit3("Sun color", &settings.sunColor.x);
    ImGui::EndDisabled();
    changed |= ImGui::SliderFloat("Haze", &settings.haze, 0.0f, 10.0f, "%.2f", kLogClamp);
    ImGui::SetItemTooltip("Aerosols in the air: a whiter sky, a softer glow around the sun and a warmer low sun.");

    changed |= ImGui::Checkbox("Fog", &settings.fog);
    ImGui::BeginDisabled(!settings.fog);
    changed |= ImGui::SliderFloat("Fog density", &settings.fogDensity, 0.0f, 100.0f, "%.2f", kLogClamp);
    ImGui::EndDisabled();
    changed |= ImGui::SliderFloat("View distance", &settings.viewDistance, 100.0f, 20000.0f, "%.0f m", kLogClamp);
    return changed;
}

} // namespace

bool drawGraphicsSettings(GraphicsSettings& settings, const RenderCapabilities& caps)
{
    bool changed = false;
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Quality preset");
    static const char* const presets[] = { "Low", "Medium", "High", "Ultra" };
    for (int i = 0; i < IM_ARRAYSIZE(presets); ++i) {
        ImGui::SameLine();
        if (ImGui::Button(presets[i])) {
            applyPreset(settings, static_cast<QualityPreset>(i), caps);
            changed = true;
        }
    }

    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen))
        changed |= drawDisplay(settings, caps);
    if (ImGui::CollapsingHeader("Shadows", ImGuiTreeNodeFlags_DefaultOpen))
        changed |= drawShadows(settings);
    if (ImGui::CollapsingHeader("Lighting and effects", ImGuiTreeNodeFlags_DefaultOpen))
        changed |= drawLighting(settings);
    if (ImGui::CollapsingHeader("Environment", ImGuiTreeNodeFlags_DefaultOpen))
        changed |= drawEnvironment(settings);

    ImGui::PopItemWidth();
    ImGui::Spacing();
    if (ImGui::Button("Reset to defaults") && !(settings == GraphicsSettings{})) {
        settings = GraphicsSettings{};
        if (settings.msaaSamples > caps.maxMsaaSamples) settings.msaaSamples = caps.maxMsaaSamples;
        changed = true;
    }
    return changed;
}
