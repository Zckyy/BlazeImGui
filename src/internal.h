// Internal shared state between BlazeImGui translation units. Not part of the public API.
#pragma once
#include "blaze/blaze.h"

#include <imgui_internal.h>
#include <string>
#include <unordered_map>

namespace blaze::detail {

// Fonts loaded by LoadFonts(). Any can be null (falls back to default font).
struct Fonts {
    ImFont* regular  = nullptr;
    ImFont* semibold = nullptr;
    ImFont* title    = nullptr;
    ImFont* large    = nullptr;
    ImFont* mono     = nullptr;
    bool    iconsLoaded = false;
};
Fonts& GetFonts();

// Frame-rate independent animation toward a target, keyed by ImGuiID.
float Animate(ImGuiID id, float target, float speed = 14.0f);

// Built-in panel registration (one per file in src/panels/).
void RegisterPerformancePanel();
void RegisterDebugPanel();
void RegisterConfigPanel();
void RegisterSettingsPanel();
void DrawDebugToolWindows();   // ImGui demo/metrics/style windows toggled from the Debug page

// Shared settings bound into the global config by blaze.cpp.
struct CoreSettings {
    int   themeMode = 0;       // theme::Mode
    ImVec4 accent{};
    float uiScale = 1.0f;
    int   toggleKey = 0x2D;
    bool  blockInput = true;
    bool  softwareCursor = false;
    bool  showFpsInHeader = true;
    bool  animations = true;
    bool  autoSave = true;
    std::string lastPanel;
    std::string lastProfile = "default";
};
CoreSettings& Settings();

// Called by the settings page after editing theme fields.
void ApplyThemeSettings();

// Panel search query from the header box (lowercase).
const std::string& SearchQuery();

// Blur module (blur_dx11.cpp, or blur_none.cpp when built with BLAZE_NO_DX11).
namespace blur {
bool  Init(ID3D11Device* device, ID3D11DeviceContext* context);
bool  Available();   // Init succeeded and the shaders are usable
void  Shutdown();
void  ReleaseTargets();
// Captures `target`'s contents, blurs them (acrylic: blur + saturation + tint + noise)
// and composites the result back over `target` with alpha = visibility.
// All D3D11 state touched is saved and restored.
bool  Apply(ID3D11RenderTargetView* target, const BlurSettings& s, const ImVec4& tint, float visibility);
}

} // namespace blaze::detail
