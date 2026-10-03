// =============================================================================
// BlazeImGui panel template - copy into your project and rename.
//
// 1. Replace "mygame.example" / "Example" with your page id and title.
// 2. Call RegisterExamplePanel() once after blaze::Initialize().
// 3. Variables bound with config::Bind are saved/loaded with config profiles.
// =============================================================================
#include <blaze/blaze.h>

namespace {

// Persistent settings (bound to config keys below). Keep them alive for the
// whole program - statics or members of a long-lived object.
bool   g_enabled   = true;
float  g_intensity = 0.5f;
int    g_mode      = 0;
int    g_hotkey    = 0x46; // 'F'
ImVec4 g_color     = ImVec4(0.30f, 0.55f, 1.00f, 1.00f);

void DrawExamplePanel() {
    using namespace blaze;
    using namespace blaze::ui;
    const auto& c = theme::Colors();

    if (BeginCard("Feature", icons::Lightbulb, "Short description of what this group controls")) {
        Toggle("Enabled", &g_enabled, "Optional one-line description under the label");
        SliderFloat("Intensity", &g_intensity, 0.0f, 1.0f, "%.2f");
        const char* modes[] = { "Off", "Low", "High" };
        Combo("Mode", &g_mode, modes, IM_ARRAYSIZE(modes));
        ColorEdit("Colour", &g_color);
        KeyBind("Hotkey", &g_hotkey);
    }
    EndCard(); // always call, even if BeginCard returned false

    if (BeginCard("Actions", icons::Play)) {
        if (Button("Do the thing", ButtonKind::Primary)) {
            Log::Info("Did the thing at intensity %.2f", g_intensity);
            Notify("Done!", ToastKind::Success);
        }
        ImGui::SameLine();
        if (Button("Reset", ButtonKind::Subtle)) config::ResetToDefaults();
        ImGui::Dummy(ImVec2(0, 4 * GetUIScale()));
        TextMuted("Status: %s", g_enabled ? "running" : "stopped");
        ProgressBar(g_intensity, ImVec2(-1, 6 * GetUIScale()), &c.accent);
    }
    EndCard();
}

} // namespace

void RegisterExamplePanel() {
    using namespace blaze;

    config::Bind("example.enabled", &g_enabled);
    config::Bind("example.intensity", &g_intensity);
    config::Bind("example.mode", &g_mode);
    config::Bind("example.color", &g_color);
    config::BindKey("example.hotkey", &g_hotkey);

    Panel p;
    p.id       = "mygame.example";      // unique + stable
    p.title    = "Example";
    p.subtitle = "What this page is for";
    p.category = "Game";                // Overview | Game | Debug | Tools | System | anything
    p.icon     = icons::Lightbulb;
    p.order    = 50;                    // lower = higher in its category
    p.draw     = DrawExamplePanel;
    RegisterPanel(p);

    // Optional extras
    debug::RegisterCommand("example.run", "Run the example action", [](const debug::Args&) {
        Log::Info("example.run executed");
    });
    debug::Watch("Example intensity", [] { return debug::Fmt("%.2f", g_intensity); }, "Example");
}
