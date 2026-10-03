// Built-in "Settings" page: appearance, acrylic blur, overlay, input, about.
#include "internal.h"

#include <cmath>

namespace blaze::detail {
namespace {

void AccentSwatches() {
    const auto& c = theme::Colors();
    const float S = GetUIScale();
    auto& st = Settings();
    int count = 0;
    const theme::AccentPreset* presets = theme::AccentPresets(&count);

    // Row label like the other setting rows
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float r = 11 * S, gap = 8 * S;
    float rowH = 46 * S;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddLine(p, ImVec2(p.x + w, p.y), theme::U32(c.border));
    float lh = ImGui::GetTextLineHeight();
    dl->AddText(ImVec2(p.x, p.y + (rowH - lh) * 0.5f), theme::U32(c.text), "Accent colour");

    float total = count * (r * 2 + gap) - gap;
    float x = p.x + w - total;
    for (int i = 0; i < count; ++i) {
        ImVec2 center(x + r, p.y + rowH * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(center.x - r, center.y - r));
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("##accent", ImVec2(r * 2, r * 2));
        bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        const ImVec4& col = presets[i].color;
        bool selected = std::fabs(col.x - st.accent.x) + std::fabs(col.y - st.accent.y) + std::fabs(col.z - st.accent.z) < 0.01f;
        dl->AddCircleFilled(center, r - (hovered ? 0 : 1 * S), ImGui::ColorConvertFloat4ToU32(col), 24);
        if (selected) {
            dl->AddCircle(center, r + 3 * S, theme::U32(c.text, 0.85f), 24, 2 * S);
            if (GetFonts().iconsLoaded) {
                ImVec2 ts = ImGui::CalcTextSize(icons::Checkmark);
                dl->AddText(ImVec2(center.x - ts.x * 0.5f, center.y - ts.y * 0.5f), IM_COL32_WHITE, icons::Checkmark);
            }
        }
        if (hovered) ImGui::SetTooltip("%s", presets[i].name);
        if (clicked) { st.accent = col; ApplyThemeSettings(); }
        x += r * 2 + gap;
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, rowH));
}

void Draw() {
    using namespace ui;
    const auto& c = theme::Colors();
    const float S = GetUIScale();
    auto& st = Settings();
    auto& blur = Blur();
    auto& ov = Overlay();

    // ---- Appearance ----
    if (BeginCard("Appearance", icons::Brush, "Theme, accent colour and scaling")) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        const char* modes[] = { "Dark", "Light", "System" };
        float segW = 260 * S, segH = ImGui::GetFrameHeight() + 2 * S;
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x, p.y + (segH - ImGui::GetTextLineHeight()) * 0.5f),
                                            theme::U32(c.text), "Theme");
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - segW, p.y));
        if (Segmented("##theme", &st.themeMode, modes, 3, segW)) ApplyThemeSettings();
        ImGui::Dummy(ImVec2(0, 2 * S));

        AccentSwatches();
        if (ColorEdit("Custom accent", &st.accent, "Pick any colour; hover/pressed shades are derived automatically"))
            ApplyThemeSettings();

        // Apply on release: rebuilding fonts mid-drag would make the slider jump under the cursor.
        static float pendingPct = 100.0f;
        static bool  editingScale = false;
        if (!editingScale) pendingPct = std::round(st.uiScale * 100.0f);
        if (SliderFloat("Interface scale", &pendingPct, 75, 200, "%.0f%%", "Applied when you release the slider"))
            editingScale = true;
        if (editingScale && !ImGui::IsMouseDown(0)) { SetUIScale(std::round(pendingPct) / 100.0f); editingScale = false; }
        Toggle("Animations", &st.animations, "Smooth transitions for hover, navigation and theme changes");
        Toggle("FPS in header", &st.showFpsInHeader);
    }
    EndCard();

    // ---- Blur ----
    if (BeginCard("Acrylic background", icons::View, "Windows 11 style blur behind the menu while it is open")) {
        if (!BlurAvailable())
            TextMuted("Not available with this renderer. The menu uses a translucent background instead.");
        Toggle("Blur background", &blur.enabled, "Captures the game frame and blurs it on the GPU");
        if (!blur.enabled) ImGui::BeginDisabled();
        SliderInt("Blur strength", &blur.strength, 1, 6, "%d", "Each step doubles the blur radius");
        SliderFloat("Tint", &blur.tintAmount, 0.0f, 0.9f, "%.2f", "How much the theme colour shows over the blur");
        SliderFloat("Luminosity", &blur.dim, 0.0f, 0.6f, "%.2f", "Darkens (dark mode) or brightens (light mode) the background");
        SliderFloat("Saturation", &blur.saturation, 0.0f, 2.0f, "%.2f");
        SliderFloat("Grain", &blur.noise, 0.0f, 0.12f, "%.3f", "Subtle noise texture, as in Windows acrylic");
        if (!blur.enabled) ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0, 2 * S));
        if (Button("Restore defaults", ButtonKind::Subtle)) { bool e = blur.enabled; blur = BlurSettings{}; blur.enabled = e; }
    }
    EndCard();

    // ---- Overlay ----
    if (BeginCard("Performance overlay", icons::Speed, "Small HUD that stays visible when the menu is closed")) {
        Toggle("Show overlay", &ov.enabled);
        const char* corners[] = { "Top left", "Top right", "Bottom left", "Bottom right" };
        Combo("Position", &ov.corner, corners, 4);
        Toggle("Frame time graph", &ov.showGraph);
        Toggle("CPU and memory", &ov.showSystem);
    }
    EndCard();

    // ---- Input ----
    if (BeginCard("Input", icons::Keyboard)) {
        KeyBind("Menu toggle key", &st.toggleKey, "Opens and closes this menu");
        Toggle("Block game input while open", &st.blockInput, "Mouse and keyboard messages are not forwarded to the game");
        Toggle("Software cursor", &st.softwareCursor, "Draw a cursor with ImGui (for games that hide the OS cursor)");
    }
    EndCard();

    // ---- About ----
    if (BeginCard("About", icons::Info)) {
        KeyValue("BlazeImGui", "%s", kVersion);
        KeyValue("Dear ImGui", "%s", IMGUI_VERSION);
        KeyValue("Renderer", "Direct3D 11");
        KeyValue("Config folder", "%s", config::Directory().c_str());
        ImGui::Dummy(ImVec2(0, 2 * S));
        ImGui::PushStyleColor(ImGuiCol_Text, c.textMuted);
        ImGui::TextWrapped("Tip: every setting here is also a console command or config key. Type 'blaze.vars' in the Debug console to list them.");
        ImGui::PopStyleColor();
    }
    EndCard();
}

} // namespace

void RegisterSettingsPanel() {
    Panel p;
    p.id = "blaze.settings";
    p.title = "Settings";
    p.subtitle = "Appearance, acrylic blur, overlay and input";
    p.category = "System";
    p.icon = icons::Settings;
    p.order = 20;
    p.draw = Draw;
    RegisterPanel(p);
}

} // namespace blaze::detail
