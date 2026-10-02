#pragma once
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include "imgui.h"

namespace EditorStyle {

// The UI is rendered into an sRGB swapchain, which treats vertex colors as linear and brightens
// them. Colors below are authored in sRGB and converted so they appear as designed.
inline ImVec4 lin(const ImVec4& c) {
    auto f = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    return ImVec4(f(c.x), f(c.y), f(c.z), c.w);
}

// Accent colors shared by the editor widgets.
inline const ImVec4 kAccent = lin(ImVec4(0.26f, 0.52f, 0.92f, 1.00f));
inline const ImVec4 kAccentHover = lin(ImVec4(0.35f, 0.60f, 0.97f, 1.00f));
inline const ImVec4 kAccentActive = lin(ImVec4(0.20f, 0.44f, 0.82f, 1.00f));
inline const ImVec4 kTextDim = lin(ImVec4(0.58f, 0.60f, 0.64f, 1.00f));
inline const ImVec4 kAxisX = lin(ImVec4(0.78f, 0.22f, 0.20f, 1.00f));
inline const ImVec4 kAxisY = lin(ImVec4(0.30f, 0.64f, 0.22f, 1.00f));
inline const ImVec4 kAxisZ = lin(ImVec4(0.20f, 0.40f, 0.80f, 1.00f));
inline const ImVec4 kHighlight = lin(ImVec4(0.95f, 0.78f, 0.30f, 1.00f));
inline const ImVec4 kError = lin(ImVec4(0.95f, 0.40f, 0.35f, 1.00f));
inline const ImVec4 kDanger = lin(ImVec4(0.70f, 0.25f, 0.22f, 1.00f));

inline void loadFonts(ImGuiIO& io) {
    // Segoe UI is the Windows UI font; fall back to Arial, then ImGui's built-in font.
    const char* candidates[] = { "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf" };
    for (const char* path : candidates) {
        if (std::filesystem::exists(path)) {
            io.Fonts->AddFontFromFileTTF(path, 17.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic());
            return;
        }
    }
    io.Fonts->AddFontDefault();
}

inline void apply() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);

    style.WindowPadding = ImVec2(8, 8);
    style.FramePadding = ImVec2(6, 4);
    style.CellPadding = ImVec2(6, 3);
    style.ItemSpacing = ImVec2(8, 5);
    style.ItemInnerSpacing = ImVec2(5, 4);
    style.IndentSpacing = 16.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.TabBorderSize = 0.0f;

    style.WindowRounding = 4.0f;
    style.ChildRounding = 3.0f;
    style.FrameRounding = 3.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 3.0f;
    style.TabRounding = 3.0f;

    style.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.SeparatorTextBorderSize = 1.0f;

    ImVec4* c = style.Colors;
    const ImVec4 bg0 = ImVec4(0.110f, 0.114f, 0.125f, 1.00f); // deepest (docking empty, title)
    const ImVec4 bg1 = ImVec4(0.145f, 0.149f, 0.161f, 1.00f); // panels
    const ImVec4 bg2 = ImVec4(0.196f, 0.200f, 0.216f, 1.00f); // frames
    const ImVec4 bg3 = ImVec4(0.251f, 0.255f, 0.275f, 1.00f); // hovered frames
    const ImVec4 border = ImVec4(0.070f, 0.072f, 0.080f, 1.00f);
    // sRGB versions of the k* constants (the palette is converted to linear at the end).
    const ImVec4 accent = ImVec4(0.26f, 0.52f, 0.92f, 1.00f);
    const ImVec4 accentHover = ImVec4(0.35f, 0.60f, 0.97f, 1.00f);
    const ImVec4 accentActive = ImVec4(0.20f, 0.44f, 0.82f, 1.00f);
    const ImVec4 textDim = ImVec4(0.58f, 0.60f, 0.64f, 1.00f);

    c[ImGuiCol_Text] = ImVec4(0.88f, 0.89f, 0.91f, 1.00f);
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_WindowBg] = bg1;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.125f, 0.129f, 0.141f, 1.00f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = bg2;
    c[ImGuiCol_FrameBgHovered] = bg3;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.290f, 0.294f, 0.318f, 1.00f);
    c[ImGuiCol_TitleBg] = bg0;
    c[ImGuiCol_TitleBgActive] = bg0;
    c[ImGuiCol_TitleBgCollapsed] = bg0;
    c[ImGuiCol_MenuBarBg] = bg0;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = bg3;
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.34f, 0.35f, 0.37f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.40f, 0.41f, 0.44f, 1.00f);
    c[ImGuiCol_CheckMark] = accentHover;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHover;
    c[ImGuiCol_Button] = bg2;
    c[ImGuiCol_ButtonHovered] = bg3;
    c[ImGuiCol_ButtonActive] = accentActive;
    c[ImGuiCol_Header] = ImVec4(0.26f, 0.52f, 0.92f, 0.35f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.26f, 0.52f, 0.92f, 0.22f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.26f, 0.52f, 0.92f, 0.50f);
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_SeparatorActive] = accentHover;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.26f, 0.52f, 0.92f, 0.50f);
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = bg0;
    c[ImGuiCol_TabHovered] = bg3;
    c[ImGuiCol_TabSelected] = bg1;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = bg0;
    c[ImGuiCol_TabDimmedSelected] = bg1;
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_DockingPreview] = ImVec4(0.26f, 0.52f, 0.92f, 0.60f);
    c[ImGuiCol_DockingEmptyBg] = bg0;
    c[ImGuiCol_PlotLines] = accentHover;
    c[ImGuiCol_PlotHistogram] = accent;
    c[ImGuiCol_TableHeaderBg] = bg0;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.10f, 0.10f, 0.11f, 1.00f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.008f); // blended in linear space, so keep it subtle
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.26f, 0.52f, 0.92f, 0.40f);
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);

    for (int i = 0; i < ImGuiCol_COUNT; ++i)
        c[i] = lin(c[i]);
}

// Label on the left, widget filling the rest of the row (Unity/Unreal inspector style).
inline void propertyLabel(const char* label, float labelWidth = 110.0f) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-FLT_MIN);
}

// Three colored X/Y/Z drag fields; clicking an axis button resets that component.
inline bool vec3Control(const char* label, float* values, float resetValue, float speed,
    float minValue = 0.0f, float maxValue = 0.0f, float labelWidth = 110.0f) {
    bool changed = false;
    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(labelWidth);

    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float lineHeight = ImGui::GetFrameHeight();
    const ImVec2 buttonSize(lineHeight, lineHeight);
    const float fieldWidth = (ImGui::GetContentRegionAvail().x - 3 * buttonSize.x - 2 * spacing * 2) / 3.0f;

    const char* axes[3] = { "X", "Y", "Z" };
    const ImVec4 colors[3] = { kAxisX, kAxisY, kAxisZ };
    for (int i = 0; i < 3; ++i) {
        if (i > 0) ImGui::SameLine(0, spacing * 2);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_Button, colors[i]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(colors[i].x + 0.1f, colors[i].y + 0.1f, colors[i].z + 0.1f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors[i]);
        if (ImGui::Button(axes[i], buttonSize)) {
            values[i] = resetValue;
            changed = true;
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reset %s to %.2f", axes[i], resetValue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::max(fieldWidth, 20.0f));
        ImGui::PushID(i);
        changed |= ImGui::DragFloat("##v", &values[i], speed, minValue, maxValue, "%.2f");
        ImGui::PopID();
        ImGui::PopStyleVar();
    }
    ImGui::PopID();
    return changed;
}

// Two-column read-only key/value row inside an active table.
inline void keyValueRow(const char* key, const char* value) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(kTextDim, "%s", key);
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", (value && *value) ? value : "-");
}

} // namespace EditorStyle
