#include "blaze/theme.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace blaze::theme {
namespace {

constexpr ImVec4 Hex(unsigned rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}

const AccentPreset kAccents[] = {
    { "Azure",   Hex(0x4C8DFF) },
    { "Ember",   Hex(0xFF7043) },
    { "Violet",  Hex(0x8B5CF6) },
    { "Emerald", Hex(0x10B981) },
    { "Rose",    Hex(0xF43F5E) },
    { "Amber",   Hex(0xF59E0B) },
    { "Cyan",    Hex(0x06B6D4) },
    { "Slate",   Hex(0x94A3B8) },
};

Mode    g_mode = Mode::Dark;
ImVec4  g_accent = kAccents[0].color;
bool    g_systemDark = true;
float   g_systemPollTimer = 0.0f;
Palette g_current{};
Palette g_target{};
bool    g_initialized = false;

float Luminance(const ImVec4& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

ImVec4 Shade(const ImVec4& c, float amount) { // amount > 0 lightens, < 0 darkens
    if (amount >= 0) return ImVec4(c.x + (1 - c.x) * amount, c.y + (1 - c.y) * amount, c.z + (1 - c.z) * amount, c.w);
    float k = 1 + amount;
    return ImVec4(c.x * k, c.y * k, c.z * k, c.w);
}

bool ReadSystemDark() {
    DWORD value = 0, size = sizeof(value);
    LSTATUS st = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return st == ERROR_SUCCESS ? value == 0 : true;
}

Palette Build(bool dark, const ImVec4& accent) {
    Palette p{};
    if (dark) {
        p.background    = Hex(0x1B1B1F, 0.84f);
        p.sidebar       = ImVec4(0, 0, 0, 0.18f);
        p.surface       = ImVec4(1, 1, 1, 0.045f);
        p.surfaceHover  = ImVec4(1, 1, 1, 0.075f);
        p.surfaceActive = ImVec4(1, 1, 1, 0.11f);
        p.control       = ImVec4(1, 1, 1, 0.075f);
        p.controlHover  = ImVec4(1, 1, 1, 0.11f);
        p.border        = ImVec4(1, 1, 1, 0.085f);
        p.text          = Hex(0xF4F4F6);
        p.textMuted     = Hex(0xA3A3AD);
        p.textDisabled  = Hex(0x62626B);
        p.accent        = accent;
        p.accentHover   = Shade(accent, 0.12f);
        p.accentActive  = Shade(accent, -0.12f);
        p.success       = Hex(0x34D399);
        p.warning       = Hex(0xFBBF24);
        p.error         = Hex(0xF87171);
        p.info          = Hex(0x60A5FA);
        p.shadow        = ImVec4(0, 0, 0, 0.45f);
    } else {
        p.background    = Hex(0xF5F5F8, 0.86f);
        p.sidebar       = ImVec4(0, 0, 0, 0.035f);
        p.surface       = ImVec4(1, 1, 1, 0.72f);
        p.surfaceHover  = ImVec4(0, 0, 0, 0.045f);
        p.surfaceActive = ImVec4(0, 0, 0, 0.08f);
        p.control       = ImVec4(0, 0, 0, 0.06f);
        p.controlHover  = ImVec4(0, 0, 0, 0.095f);
        p.border        = ImVec4(0, 0, 0, 0.09f);
        p.text          = Hex(0x18181B);
        p.textMuted     = Hex(0x5B5B66);
        p.textDisabled  = Hex(0xA1A1AA);
        // Slightly deeper accent in light mode for contrast against pale surfaces.
        p.accent        = Shade(accent, -0.08f);
        p.accentHover   = Shade(accent, 0.06f);
        p.accentActive  = Shade(accent, -0.22f);
        p.success       = Hex(0x059669);
        p.warning       = Hex(0xD97706);
        p.error         = Hex(0xDC2626);
        p.info          = Hex(0x2563EB);
        p.shadow        = ImVec4(0, 0, 0, 0.18f);
    }
    p.accentText = Luminance(p.accent) > 0.62f ? Hex(0x111114) : Hex(0xFFFFFF);
    return p;
}

void ApplyToImGui(const Palette& p) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4* c = s.Colors;
    const bool dark = IsDark();
    // Opaque-ish popup color so menus/tooltips stay readable over anything.
    ImVec4 popup = dark ? Hex(0x26262B, 0.98f) : Hex(0xFBFBFD, 0.98f);

    c[ImGuiCol_Text]                  = p.text;
    c[ImGuiCol_TextDisabled]          = p.textDisabled;
    c[ImGuiCol_WindowBg]              = p.background;
    c[ImGuiCol_ChildBg]               = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]               = popup;
    c[ImGuiCol_Border]                = p.border;
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]               = p.control;
    c[ImGuiCol_FrameBgHovered]        = p.controlHover;
    c[ImGuiCol_FrameBgActive]         = p.surfaceActive;
    c[ImGuiCol_TitleBg]               = p.background;
    c[ImGuiCol_TitleBgActive]         = p.background;
    c[ImGuiCol_TitleBgCollapsed]      = p.background;
    c[ImGuiCol_MenuBarBg]             = p.sidebar;
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]         = WithAlpha(p.text, 0.16f);
    c[ImGuiCol_ScrollbarGrabHovered]  = WithAlpha(p.text, 0.28f);
    c[ImGuiCol_ScrollbarGrabActive]   = WithAlpha(p.text, 0.38f);
    c[ImGuiCol_CheckMark]             = p.accentText;
    c[ImGuiCol_SliderGrab]            = p.accent;
    c[ImGuiCol_SliderGrabActive]      = p.accentActive;
    c[ImGuiCol_Button]                = p.control;
    c[ImGuiCol_ButtonHovered]         = p.controlHover;
    c[ImGuiCol_ButtonActive]          = p.surfaceActive;
    c[ImGuiCol_Header]                = WithAlpha(p.accent, 0.20f);
    c[ImGuiCol_HeaderHovered]         = WithAlpha(p.accent, 0.28f);
    c[ImGuiCol_HeaderActive]          = WithAlpha(p.accent, 0.36f);
    c[ImGuiCol_Separator]             = p.border;
    c[ImGuiCol_SeparatorHovered]      = WithAlpha(p.accent, 0.6f);
    c[ImGuiCol_SeparatorActive]       = p.accent;
    c[ImGuiCol_ResizeGrip]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]     = WithAlpha(p.accent, 0.5f);
    c[ImGuiCol_ResizeGripActive]      = p.accent;
    c[ImGuiCol_Tab]                   = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered]            = p.surfaceHover;
    c[ImGuiCol_TabSelected]           = p.surfaceActive;
    c[ImGuiCol_TabSelectedOverline]   = p.accent;
    c[ImGuiCol_TabDimmed]             = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected]     = p.surface;
    c[ImGuiCol_TabDimmedSelectedOverline] = WithAlpha(p.accent, 0.5f);
    c[ImGuiCol_PlotLines]             = p.accent;
    c[ImGuiCol_PlotLinesHovered]      = p.accentHover;
    c[ImGuiCol_PlotHistogram]         = p.accent;
    c[ImGuiCol_PlotHistogramHovered]  = p.accentHover;
    c[ImGuiCol_TableHeaderBg]         = p.surface;
    c[ImGuiCol_TableBorderStrong]     = p.border;
    c[ImGuiCol_TableBorderLight]      = WithAlpha(p.border, p.border.w * 0.6f);
    c[ImGuiCol_TableRowBg]            = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]         = WithAlpha(p.text, 0.025f);
    c[ImGuiCol_TextLink]              = p.accent;
    c[ImGuiCol_TextSelectedBg]        = WithAlpha(p.accent, 0.35f);
    c[ImGuiCol_DragDropTarget]        = p.accent;
    c[ImGuiCol_NavCursor]             = p.accent;
    c[ImGuiCol_NavWindowingHighlight] = WithAlpha(p.text, 0.7f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0, 0, 0, 0.2f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, dark ? 0.45f : 0.25f);
}

} // namespace

const AccentPreset* AccentPresets(int* count) {
    if (count) *count = int(sizeof(kAccents) / sizeof(kAccents[0]));
    return kAccents;
}

bool IsDark() { return g_mode == Mode::System ? g_systemDark : g_mode == Mode::Dark; }

void SetMode(Mode mode) {
    g_mode = mode;
    if (mode == Mode::System) g_systemDark = ReadSystemDark();
    g_target = Build(IsDark(), g_accent);
}

Mode GetMode() { return g_mode; }

void SetAccent(const ImVec4& color) {
    g_accent = ImVec4(color.x, color.y, color.z, 1.0f);
    g_target = Build(IsDark(), g_accent);
}

ImVec4 GetAccent() { return g_accent; }

const Palette& Colors() {
    if (!g_initialized) { g_target = g_current = Build(IsDark(), g_accent); g_initialized = true; }
    return g_current;
}

void Update(float dt) {
    if (!g_initialized) Colors();

    if (g_mode == Mode::System && (g_systemPollTimer -= dt) <= 0.0f) {
        g_systemPollTimer = 2.0f;
        bool d = ReadSystemDark();
        if (d != g_systemDark) { g_systemDark = d; g_target = Build(d, g_accent); }
    }

    // Exponential approach: ~95% of the way in 200 ms, frame-rate independent.
    float t = 1.0f - std::exp(-dt * 15.0f);
    constexpr int n = sizeof(Palette) / sizeof(ImVec4);
    auto* cur = reinterpret_cast<ImVec4*>(&g_current);
    auto* tgt = reinterpret_cast<const ImVec4*>(&g_target);
    for (int i = 0; i < n; ++i) cur[i] = Lerp(cur[i], tgt[i], t);

    ApplyToImGui(g_current);
}

void ApplyStyleMetrics(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    ImVec4 colors[ImGuiCol_COUNT];
    memcpy(colors, s.Colors, sizeof(colors));
    s = ImGuiStyle();
    memcpy(s.Colors, colors, sizeof(colors));

    s.WindowPadding     = ImVec2(14, 14);
    s.FramePadding      = ImVec2(10, 6);
    s.CellPadding       = ImVec2(8, 5);
    s.ItemSpacing       = ImVec2(8, 8);
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.IndentSpacing     = 18;
    s.ScrollbarSize     = 10;
    s.GrabMinSize       = 10;
    s.WindowBorderSize  = 1;
    s.ChildBorderSize   = 0;
    s.PopupBorderSize   = 1;
    s.FrameBorderSize   = 0;
    s.TabBorderSize     = 0;
    s.WindowRounding    = 12;
    s.ChildRounding     = 8;
    s.FrameRounding     = 6;
    s.PopupRounding     = 8;
    s.ScrollbarRounding = 12;
    s.GrabRounding      = 6;
    s.TabRounding       = 6;
    s.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
    s.SeparatorTextBorderSize = 1;
    s.AntiAliasedLines = s.AntiAliasedFill = true;
    s.ScaleAllSizes(scale);
}

ImU32 U32(const ImVec4& c, float alphaMul) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w * alphaMul * ImGui::GetStyle().Alpha));
}

ImVec4 Lerp(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

ImVec4 WithAlpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

} // namespace blaze::theme
