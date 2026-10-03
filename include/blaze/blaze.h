// =============================================================================
// blaze/blaze.h - BlazeImGui public API (include this one header).
//
// A drop-in, modern Dear ImGui debug menu for DirectX 11 games and tools:
// Windows 11 acrylic background blur, light/dark themes, JSON config profiles,
// performance + debug pages, and a simple panel registry for your own pages.
//
// ---- Integration A: you own the render loop --------------------------------------
//
//     blaze::InitInfo info;
//     info.device = device; info.context = context; info.swapChain = swapChain;
//     info.hwnd = hwnd; info.appName = "My Game";
//     blaze::Initialize(info);
//
//     // WndProc:
//     if (blaze::WndProcHandler(hwnd, msg, wParam, lParam)) return 1;
//
//     // Each frame, after rendering your scene to the back buffer:
//     blaze::NewFrame();          // starts the ImGui frame and builds the menu
//     /* optional: your own ImGui::Begin/End windows here */
//     blaze::Render(backBufferRTV);  // blur + draw. Then Present().
//
//     // On WM_SIZE: blaze::OnResizeBegin(); ResizeBuffers(); blaze::OnResizeEnd();
//     blaze::Shutdown();
//
// ---- Integration B: hooked IDXGISwapChain::Present -------------------------------
//     Same calls, from inside your Present hook. Create the RTV from the swap
//     chain's back buffer each frame (or cache it and release on ResizeBuffers).
//     BlazeImGui saves and restores all D3D11 pipeline state it touches.
//
// ---- Integration C: you already manage ImGui ------------------------------------
//     Set info.manageImGui = false. Then call blaze::DrawUI() between your
//     ImGui::NewFrame() and ImGui::Render(), and blaze::RenderBlur(rtv) just
//     before ImGui_ImplDX11_RenderDrawData(). Blaze fonts are not loaded in
//     this mode (call blaze::LoadFonts() yourself before building the atlas).
//
//     Any renderer works in this mode (e.g. D3D12): leave device/context null
//     and skip RenderBlur. The menu then draws without acrylic blur. Building
//     with -DBLAZE_RENDERER_DX11=OFF drops the D3D11 code and dependencies.
// =============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <vector>
#include <string>
#include <imgui.h>

#include "blaze/icons.h"
#include "blaze/theme.h"
#include "blaze/widgets.h"
#include "blaze/config.h"
#include "blaze/perf.h"
#include "blaze/debug.h"

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct IDXGISwapChain;
struct IDXGIAdapter;
struct HWND__;
typedef HWND__* HWND;

namespace blaze {

constexpr const char* kVersion = "1.1.0";

struct InitInfo {
    ID3D11Device*        device    = nullptr;  // Required when manageImGui = true; optional in Integration C (enables blur)
    ID3D11DeviceContext* context   = nullptr;  // Required with device (immediate context)
    IDXGISwapChain*      swapChain = nullptr;  // Optional - used for back buffer size/format & GPU name
    HWND                 hwnd      = nullptr;  // Required when manageImGui = true; also used for DPI + toggle-key focus
    IDXGIAdapter*        adapter   = nullptr;  // Optional - GPU name/VRAM when there is no D3D11 device (e.g. D3D12 host)

    const char* appName   = "BlazeImGui";      // Shown in the header
    const char* configDir = nullptr;           // Default: %APPDATA%/<appName>
    bool  manageImGui     = true;              // Create ImGui context + Win32/DX11 backends
    bool  startOpen       = true;              // Menu visible after Initialize
    int   toggleKey       = 0x2D;              // VK_INSERT. Saved globally; user can rebind
    bool  blockInputWhenOpen = true;           // WndProcHandler swallows input while menu open
    bool  registerBuiltinPanels = true;        // Performance / Debug / Configs / Settings
};

bool Initialize(const InitInfo& info);
void Shutdown();
bool IsInitialized();

// Integration A/B frame calls --------------------------------------------------------
void NewFrame();                                  // Backends NewFrame + ImGui::NewFrame + DrawUI
void Render(ID3D11RenderTargetView* target);      // RenderBlur + ImGui::Render + RenderDrawData

// Integration C frame calls ----------------------------------------------------------
void DrawUI();                                    // Builds menu, overlay, toasts
void RenderBlur(ID3D11RenderTargetView* target);  // Captures + blurs the target's contents

// Window messages. Returns true if the message was consumed (don't pass to the game).
bool WndProcHandler(HWND hwnd, unsigned int msg, uintptr_t wParam, intptr_t lParam);

// Swap chain resize. Call Begin before IDXGISwapChain::ResizeBuffers, End after.
void OnResizeBegin();
void OnResizeEnd();

// Integration C: returns true once after the user changes the interface scale. Rebuild
// fonts before your backend's NewFrame (wait for the GPU first if the old font texture
// may still be in flight):
//     if (blaze::ConsumeFontRebuild()) {
//         blaze::LoadFonts(blaze::GetUIScale());
//         ImGui_ImplDX12_InvalidateDeviceObjects();   // or your backend's equivalent
//     }
// Always false when Blaze manages ImGui (it rebuilds fonts itself).
bool ConsumeFontRebuild();

// Menu visibility ------------------------------------------------------------------
void SetMenuOpen(bool open);
bool IsMenuOpen();
// Current toggle key (Win32 VK code; user-rebindable). Blaze polls it with
// GetAsyncKeyState; a hooked WndProc can use this to hide the key from the game.
int  GetToggleKey();
void ToggleMenu();
// 0..1 animation value - use to fade your own overlays with the menu.
float MenuVisibility();

// Panels (pages in the sidebar) ----------------------------------------------------
struct Panel {
    std::string id;          // Unique, stable (used for navigation + saving last page)
    std::string title;       // Sidebar + page header text
    std::string subtitle;    // Optional page header description
    std::string category;    // Sidebar group, e.g. "Game", "Tools". Empty = "General"
    const char* icon = nullptr;           // blaze::icons::* or any UTF-8 string
    int         order = 100;              // Sort key within category (lower first)
    std::function<void()> draw;           // Called inside the scrolling page body
};
void RegisterPanel(const Panel& panel);
void UnregisterPanel(const std::string& id);
void SelectPanel(const std::string& id);
const std::string& SelectedPanel();
// Order of categories in the sidebar. Unlisted categories are appended alphabetically.
void SetCategoryOrder(const std::vector<std::string>& categories);

// Notifications ------------------------------------------------------------------
enum class ToastKind { Info, Success, Warning, Error };
void Notify(const char* message, ToastKind kind = ToastKind::Info, float seconds = 3.0f);

// Acrylic blur settings (also editable in Settings > Appearance) --------------------
struct BlurSettings {
    bool   enabled    = true;
    int    strength   = 4;      // Downsample passes 1..6 (radius roughly doubles per step)
    float  tintAmount = 0.35f;  // Mix of palette tint over the blurred image
    float  noise      = 0.015f; // Grain amount (Win11 acrylic look)
    float  saturation = 1.25f;
    float  dim        = 0.10f;  // Extra darkening (dark mode) / lightening (light mode)
};
BlurSettings& Blur();
// False when there is no D3D11 device (Integration C on another renderer) or the shaders failed.
bool BlurAvailable();

// Overlay: small always-on performance HUD (works with menu closed).
enum class OverlayCorner : int { TopLeft = 0, TopRight, BottomLeft, BottomRight };
struct OverlaySettings {
    bool enabled = false;
    int  corner  = (int)OverlayCorner::TopRight;
    bool showGraph = true;
    bool showSystem = false;
};
OverlaySettings& Overlay();

// Global UI scale (fonts rebuilt next frame). 1.0 = 100 %.
void  SetUIScale(float scale);
float GetUIScale();

// Loads Segoe UI + icon fonts into the current ImGui font atlas (manageImGui does this).
void LoadFonts(float scale = 1.0f);

// Snapshot of all inspectable state as JSON (perf, config vars, recent log).
// Handy for bug reports and automated tooling. Also exposed as `blaze.dump` command.
std::string DumpStateJson();

// Seconds since Initialize().
double Time();

} // namespace blaze
