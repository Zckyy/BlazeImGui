// =============================================================================
// blaze/theme.h - Semantic color palette, light/dark modes and accent colors.
//
// All BlazeImGui widgets read colors from blaze::theme::Colors() rather than
// ImGui's style, so custom panels should do the same to look native:
//
//     auto& c = blaze::theme::Colors();
//     ImGui::PushStyleColor(ImGuiCol_Text, c.textMuted);
//
// Switching mode or accent cross-fades smoothly over ~200 ms.
// =============================================================================
#pragma once
#include <imgui.h>

namespace blaze::theme {

enum class Mode : int { Dark = 0, Light = 1, System = 2 };

// Semantic palette. Every field is a full RGBA color.
struct Palette {
    ImVec4 background;     // Main window fill (alpha < 1 lets the acrylic blur show through)
    ImVec4 sidebar;        // Navigation rail fill
    ImVec4 surface;        // Cards / grouped content
    ImVec4 surfaceHover;   // Hovered card / row
    ImVec4 surfaceActive;  // Pressed / selected row
    ImVec4 control;        // Input fields, slider tracks, unchecked toggles
    ImVec4 controlHover;
    ImVec4 border;         // Hairline borders
    ImVec4 text;           // Primary text
    ImVec4 textMuted;      // Secondary text, labels
    ImVec4 textDisabled;
    ImVec4 accent;         // Brand / selection color
    ImVec4 accentHover;
    ImVec4 accentActive;
    ImVec4 accentText;     // Text drawn on top of `accent`
    ImVec4 success;
    ImVec4 warning;
    ImVec4 error;
    ImVec4 info;
    ImVec4 shadow;
};

// Built-in accent presets (index into AccentPresets()).
struct AccentPreset { const char* name; ImVec4 color; };
const AccentPreset* AccentPresets(int* count);

void   SetMode(Mode mode);
Mode   GetMode();
bool   IsDark();                       // Resolved mode (System -> actual Windows setting)
void   SetAccent(const ImVec4& color); // Any color; palette variants are derived automatically
ImVec4 GetAccent();

// Current (animated) palette. Valid after blaze::Initialize().
const Palette& Colors();

// Internal - called once per frame by blaze::NewFrame(). Applies palette to ImGuiStyle.
void Update(float dt);
// Rebuilds ImGuiStyle sizes (rounding/padding) for the given UI scale.
void ApplyStyleMetrics(float scale);

// Helpers
ImU32  U32(const ImVec4& c, float alphaMul = 1.0f);
ImVec4 Lerp(const ImVec4& a, const ImVec4& b, float t);
ImVec4 WithAlpha(const ImVec4& c, float a);

} // namespace blaze::theme
