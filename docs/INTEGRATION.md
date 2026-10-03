# BlazeImGui — Integration Guide (for AI agents and developers)

> **Audience:** an AI coding agent (or a person) adding BlazeImGui to a **game or tool project**.
> Follow the steps in order. Every code block is complete and compiles against BlazeImGui 1.0.
> This file is self-contained: copy it into the target project (e.g. `docs/BLAZEIMGUI.md`) and point
> the project's `AGENTS.md` / `CLAUDE.md` at it.
>
> Working on BlazeImGui itself instead? Read [`AGENTS.md`](../AGENTS.md).

---

## 0. Decide before writing code

Answer these from the target project's code. Ask the user only if the code doesn't make the answer clear.

| Question | How to tell | Leads to |
|---|---|---|
| Graphics API? | Look for `ID3D11Device`, `D3D11CreateDevice` | D3D11 → any mode. D3D12 (or another API) **with its own Dear ImGui backend** → Mode C without blur (see "Mode C on D3D12"). Other APIs without ImGui: stop and tell the user it isn't supported |
| Who owns the render loop? | Does the project call `IDXGISwapChain::Present` itself? | Yes → **Mode A**. A hook/DLL around someone else's `Present` → **Mode B** |
| Already uses Dear ImGui? | Look for `ImGui::CreateContext`, `ImGui_ImplDX11_Init` | Yes → **Mode C** (don't create a second context) |
| Build system? | `CMakeLists.txt` vs `.vcxproj`/`.sln` | Step 1a or 1b |
| Which thread renders? | Where `Present` is called | All BlazeImGui UI calls go on that thread |

Requirements: Windows 10/11, MSVC (VS 2022/2026), C++17 or newer, x64 recommended.

---

## 1. Add the library

### 1a. CMake projects (preferred)

```cmake
# Option 1: vendored copy (e.g. third_party/BlazeImGui)
set(BLAZE_BUILD_DEMO OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/BlazeImGui)

# Option 2: fetch
include(FetchContent)
FetchContent_Declare(blazeimgui GIT_REPOSITORY <repo-url> GIT_TAG <tag>)
set(BLAZE_BUILD_DEMO OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(blazeimgui)

target_link_libraries(my_game PRIVATE Blaze::ImGui)
```

`Blaze::ImGui` brings in Dear ImGui (pinned `v1.91.8`, with the Win32 + DX11 backends), the include
paths, and the system libraries (`d3d11 dxgi d3dcompiler psapi shell32`).

**If the project already builds its own `imgui` target** (Mode C), set `BLAZE_FETCH_IMGUI OFF`
*before* `add_subdirectory`, and make sure that target contains `imgui_impl_win32.cpp` and
`imgui_impl_dx11.cpp` and exposes its include dirs publicly. Version must be ≥ 1.91.

### 1b. Visual Studio projects without CMake

Add these to the project:

- **Sources:** everything in `BlazeImGui/src/` and `BlazeImGui/src/panels/`, plus the Dear ImGui
  sources (`imgui*.cpp`, `backends/imgui_impl_win32.cpp`, `backends/imgui_impl_dx11.cpp`). Skip the
  ImGui sources if the project already compiles them.
- **Include dirs:** `BlazeImGui/include`, `BlazeImGui/src`, the imgui root, `imgui/backends`.
- **Preprocessor:** `NOMINMAX;WIN32_LEAN_AND_MEAN;IMGUI_DEFINE_MATH_OPERATORS`
- **Libraries:** `d3d11.lib dxgi.lib d3dcompiler.lib psapi.lib shell32.lib`
- **C++ language standard:** C++17 or later. **Additional options:** `/utf-8`

---

## 2. Wire up the lifecycle

Include one header everywhere: `#include <blaze/blaze.h>`

### Mode A: the project owns the render loop

```cpp
#include <blaze/blaze.h>

// --- after the D3D11 device + swap chain exist ---
blaze::InitInfo info;
info.device    = device;        // ID3D11Device*
info.context   = context;       // immediate ID3D11DeviceContext*
info.swapChain = swapChain;     // IDXGISwapChain* (optional, recommended)
info.hwnd      = hwnd;          // game window
info.appName   = "My Game";     // also names the config folder: %APPDATA%\My Game
// info.toggleKey = VK_F1;      // default VK_INSERT; user can rebind in Settings
// info.startOpen = false;      // default true
if (!blaze::Initialize(info)) { /* log and continue without the menu */ }
RegisterDebugPages();           // your panels/binds (step 3), AFTER Initialize

// --- WndProc: first line ---
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (blaze::WndProcHandler(hwnd, msg, wParam, lParam)) return 1;   // menu consumed it
    ...
}

// --- every frame, after the scene is rendered into the back buffer, before Present ---
blaze::NewFrame();
// optional: your own ImGui::Begin()/End() windows here
blaze::Render(backBufferRTV);   // blur + menu drawn on top; binds/restores render targets itself
swapChain->Present(syncInterval, flags);

// --- swap chain resize ---
blaze::OnResizeBegin();
backBufferRTV->Release();
swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, flags);
/* recreate backBufferRTV */
blaze::OnResizeEnd();

// --- shutdown, before releasing the device ---
blaze::Shutdown();              // flushes config saves
```

### Mode B: hooked `IDXGISwapChain::Present` (DLL / injected debug tool)

BlazeImGui does **not** include a hooking library. Use whatever the project already has (MinHook,
Detours, a vtable swap, an engine callback…). The integration inside the hooks looks like this:

```cpp
#include <blaze/blaze.h>
#include <d3d11.h>

static ID3D11Device*           g_device  = nullptr;
static ID3D11DeviceContext*    g_context = nullptr;
static ID3D11RenderTargetView* g_rtv     = nullptr;
static HWND                    g_hwnd    = nullptr;
static WNDPROC                 g_origWndProc = nullptr;

static LRESULT CALLBACK HookedWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (blaze::WndProcHandler(h, m, w, l)) return 1;
    return CallWindowProcW(g_origWndProc, h, m, w, l);
}

static void CreateRTV(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back))) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!blaze::IsInitialized()) {
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_device))) {
            g_device->GetImmediateContext(&g_context);
            DXGI_SWAP_CHAIN_DESC d; sc->GetDesc(&d);
            g_hwnd = d.OutputWindow;
            g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HookedWndProc);

            blaze::InitInfo info;
            info.device = g_device; info.context = g_context; info.swapChain = sc;
            info.hwnd = g_hwnd; info.appName = "My Debug Tool";
            info.startOpen = false;
            blaze::Initialize(info);
            RegisterDebugPages();
        }
    }
    if (blaze::IsInitialized()) {
        if (!g_rtv) CreateRTV(sc);
        blaze::NewFrame();
        blaze::Render(g_rtv);   // all touched D3D11 state is restored afterwards
    }
    return OriginalPresent(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    blaze::OnResizeBegin();
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }   // MUST release before ResizeBuffers
    HRESULT hr = OriginalResizeBuffers(sc, n, w, h, f, fl);
    blaze::OnResizeEnd();                                // RTV is recreated lazily in Present
    return hr;
}

// On unload: blaze::Shutdown(); restore WndProc; release g_rtv, g_context, g_device; remove hooks.
```

Notes for Mode B:
- Many games read input via Raw Input / DirectInput, which `WndProcHandler` can't block. If the game
  still reacts while the menu is open, gate the game's input code on `blaze::IsMenuOpen()`, or turn
  on **Settings → Input → Software cursor** if the game hides the OS cursor.
- The toggle key is polled with `GetAsyncKeyState`, so it works even when the game swallows window messages.
- Pass `info.configDir` if `%APPDATA%\<appName>` isn't appropriate.

### Mode C: the project already runs Dear ImGui

```cpp
blaze::InitInfo info;
info.device = device; info.context = context; info.hwnd = hwnd;
info.appName = "My Game";
info.manageImGui = false;          // BlazeImGui won't create a context or init backends
blaze::Initialize(info);           // call AFTER ImGui::CreateContext()

// Optional: Blaze's Segoe UI + icon fonts (otherwise icons show as blanks)
// blaze::LoadFonts(1.0f);         // call before the font atlas is built

// WndProc: keep the existing ImGui_ImplWin32_WndProcHandler call, and also:
if (blaze::WndProcHandler(hwnd, msg, wParam, lParam)) return 1;

// Frame:
ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
blaze::DrawUI();                   // menu, overlay, toasts
/* existing ImGui windows */
ImGui::Render();
blaze::RenderBlur(backBufferRTV);  // BEFORE RenderDrawData, with the back buffer bound
ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
```

Note: Blaze applies its own theme to `ImGui::GetStyle()` every frame, so the project's other ImGui
windows will pick up the Blaze look as well.

#### Mode C on D3D12 (or any non-D3D11 renderer)

The acrylic blur is D3D11-only; everything else is renderer-agnostic. Configure CMake with
`BLAZE_RENDERER_DX11 OFF` and `BLAZE_FETCH_IMGUI OFF`, and provide an `imgui` target containing the
core, `imgui_impl_win32.cpp` and the project's own renderer backend (e.g. `imgui_impl_dx12.cpp`).
This removes the `d3d11`/`d3dcompiler` dependencies. Then:

```cpp
blaze::InitInfo info;
info.hwnd = hwnd;                  // DPI + toggle-key focus check
info.adapter = dxgiAdapter;        // optional: GPU name/VRAM on the Performance page
info.appName = "My Game";
info.manageImGui = false;          // device/context stay null: no blur
blaze::Initialize(info);           // AFTER ImGui::CreateContext()
blaze::LoadFonts(blaze::GetUIScale());   // before the backend builds the font texture

// Frame: the project's backend NewFrame calls, ImGui::NewFrame(), blaze::DrawUI(), ImGui::Render(),
// then the project's RenderDrawData. Do not call RenderBlur / Render.
```

When the user changes **Interface scale**, Blaze can't rebuild your backend's font texture
itself. Check once per frame, before your backend's `NewFrame`:

```cpp
if (blaze::ConsumeFontRebuild()) {
    WaitForGpuIdle();                          // the old font texture may still be in flight
    blaze::LoadFonts(blaze::GetUIScale());
    ImGui_ImplDX12_InvalidateDeviceObjects();  // NewFrame recreates it
}
```

#### Lessons from a hooked-Present D3D12 integration

These came from integrating BlazeImGui into an injected overlay DLL (D3D12, MinHook, subclassed
WndProc). They apply to any Mode B/C integration inside someone else's process.

1. **One ImGui per process.** If the host compiles ImGui sources into its own target and BlazeImGui
   fetches another copy, you get two `GImGui` globals and a menu that never appears. Build one
   `imgui` static target (core + `imgui_demo.cpp` + win32 + the host's backend), set
   `BLAZE_FETCH_IMGUI OFF` and link both the host and `Blaze::ImGui` against it.
2. **Static CRT for injected DLLs.** If the DLL uses `/MT` (no redist inside the game), set the same
   runtime on every linked target:
   `set_property(TARGET my_dll imgui blaze_imgui PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")`.
3. **Don't blanket-block input.** `blockInputWhenOpen = true` swallows all keyboard and mouse input
   while the menu is open, so the player can't move. For overlays, set it to `false` and route
   input yourself:
   - Mouse: give a click to ImGui only when `io.WantCaptureMouse`. Otherwise pass it to the game.
   - Keyboard: send keys to ImGui and hide them from the game only while `io.WantTextInput` (the
     console or search box is focused). Check this before any game hotkeys (for example a Delete
     key that unloads), so typing in a text field doesn't trigger them.
   - In a subclassed game window, don't send mouse *button* messages through
     `ImGui_ImplWin32_WndProcHandler`. Its `SetCapture`/`ReleaseCapture` calls also affect the
     game's mouse capture. Feed buttons with `io.AddMousePosEvent` + `io.AddMouseButtonEvent`
     instead.
4. **Toggle key.** Blaze polls the key with `GetAsyncKeyState`, so don't toggle the menu again in
   your WndProc. Just hide the key from the game: compare `wParam` with `blaze::GetToggleKey()`. The
   user can rebind it.
5. **Threads.** The game's WndProc often runs on a different thread from `Present`. Call all
   `blaze::` UI functions on the render thread. Once per frame, publish what the WndProc needs (menu
   open, toggle key) to atomics instead of calling `blaze::IsMenuOpen()` from the WndProc.
6. **Pages that show per-frame data.** Store a pointer to the current snapshot before `DrawUI()`,
   and have each panel's `draw` return early while it is null.
7. **Tables in pages.** The page body is about 750 px wide at 100 % scale. Prefer
   `ImGuiTableColumnFlags_WidthStretch` with weights over fixed pixel widths, which overflow and
   squeeze the stretch column to nothing.
8. **Shutdown order:** unregister your panels, commands and watches, call `blaze::Shutdown()` (this
   flushes config saves), then shut down the ImGui backends and destroy the context.

---

## 3. Add debug pages for the game

Put all of it in **one function** (e.g. `RegisterDebugPages()` in `src/debug/debug_menu.cpp`) and call
it once, right after `blaze::Initialize`. Template: [`templates/panel_template.cpp`](../templates/panel_template.cpp).

```cpp
#include <blaze/blaze.h>
#include <cstdlib>

// Persistent tweakables. Must outlive the binding (globals/statics/long-lived members).
static bool  g_godMode   = false;
static float g_timeScale = 1.0f;
static int   g_lod       = 2;

void RegisterDebugPages() {
    using namespace blaze;

    // 1) Bind values: saved/loaded with config profiles automatically.
    config::Bind("player.god_mode", &g_godMode);
    config::Bind("world.time_scale", &g_timeScale);
    config::Bind("render.lod", &g_lod);

    // 2) A page.
    Panel p;
    p.id       = "game.cheats";        // unique, stable, "game.<name>"
    p.title    = "Cheats";
    p.subtitle = "Gameplay overrides";
    p.category = "Game";                // sidebar group
    p.icon     = icons::Flash;
    p.order    = 10;
    p.draw = [] {
        using namespace blaze::ui;
        if (BeginCard("Player", icons::Person)) {
            Toggle("God mode", &g_godMode, "Player takes no damage");
        }
        EndCard();                       // ALWAYS, even if BeginCard returned false
        if (BeginCard("World", icons::Globe)) {
            SliderFloat("Time scale", &g_timeScale, 0.0f, 4.0f, "%.2fx");
            const char* lods[] = { "Low", "Medium", "High", "Ultra" };
            Combo("LOD", &g_lod, lods, 4);
        }
        EndCard();
    };
    RegisterPanel(p);

    // 3) Console commands, run from Debug → Console or blaze::debug::Execute("...").
    debug::RegisterCommand("give", "give <item> [count]", [](const debug::Args& a) {
        if (a.size() < 2) { Log::Error("usage: give <item> [count]"); return; }
        int count = a.size() > 2 ? std::atoi(a[2].c_str()) : 1;
        // Inventory::Give(a[1], count);
        Log::Info("Gave %d x %s", count, a[1].c_str());
    });

    // 4) Live watches (Debug → Watches).
    debug::Watch("Time scale", [] { return debug::Fmt("%.2f", g_timeScale); }, "World");
}
```

### Hooking up the rest of the game

```cpp
// Profiling: shows in Performance → CPU scopes
void World::Update(float dt) {
    BLAZE_PROFILE_SCOPE("World update");
    { BLAZE_PROFILE_SCOPE("AI");      ai.Tick(dt); }
    { BLAZE_PROFILE_SCOPE("Physics"); physics.Step(dt); }
}

// Counters: shows in Performance → Counters (with history graph)
blaze::perf::SetCounter("Entities", (double)entities.size());
blaze::perf::SetCounter("Draw calls", (double)renderer.drawCalls);

// Logging: thread-safe, shows in Debug → Console
blaze::Log::Info("Loaded %s in %.1f ms", level.c_str(), ms);
blaze::Log::Write(blaze::LogLevel::Warn, "net", "Packet loss %.0f%%", loss);   // with category

// Mirror an existing engine logger into the console:
// MyLogger::AddSink([](Level l, const char* msg) { blaze::Log::Write(MapLevel(l), "engine", "%s", msg); });

// Toast
blaze::Notify("Checkpoint saved", blaze::ToastKind::Success);

// React to profile loads (re-apply side effects such as resolution or vsync)
blaze::config::OnProfileLoaded([](const std::string&) { renderer.ApplySettings(); });
```

---

## 4. Widget cheat sheet (`blaze::ui`)

| Widget | Signature (returns true on change/click) |
|---|---|
| Card container | `BeginCard(title, icon = nullptr, subtitle = nullptr)` … `EndCard()` |
| Toggle row | `Toggle(label, bool*, description = nullptr)` |
| Slider rows | `SliderFloat(label, float*, min, max, fmt = "%.2f", desc)`, `SliderInt(label, int*, min, max, fmt, desc)` |
| Combo row | `Combo(label, int* current, const char* const items[], count, desc)` |
| Colour row | `ColorEdit(label, ImVec4*, desc)` |
| Keybind row | `KeyBind(label, int* vk, desc)` (Win32 VK codes) |
| Text row | `InputText(label, std::string*, hint, desc)` |
| Buttons | `Button(label, ButtonKind::Primary/Secondary/Subtle/Danger, size)`, `IconButton(icon, tooltip, active)` |
| Tabs | `Segmented(id, int* current, items, count, width)` |
| Stats | `StatTile(label, value, unit, spark*, count, color*)`, `KeyValue(key, fmt, ...)`, `Badge(text, color)` |
| Graphs | `Sparkline(...)`, `LineGraph(id, values, count, min, max, size, color, guides, n, unit)`, `ProgressBar(frac, size, color*)` |
| Layout | `BeginColumns(n, id)` / `NextColumn()` / `EndColumns()`, `SectionHeader(text)`, `TextMuted(fmt, ...)` |
| Fonts | `PushFont(Font::Regular/Semibold/Title/Large/Mono)` / `PopFont()` |

Raw `ImGui::` calls work fine inside panels too (tables, trees, plots). They inherit the theme.
Icons: `blaze::icons::*` (see `include/blaze/icons.h`): `Person, Globe, Flash, Bug, Speed, Chart,
Monitor, Clock, List, Settings, Save, Folder, Code, Console, Target, Camera, Game, Keyboard, Lightbulb, View`…

---

## 5. Rules that prevent the common mistakes

1. **Call `EndCard()` after every `BeginCard()`**, unconditionally. Don't nest cards.
2. **Only render-thread calls** for panels, `config::*`, commands and watches. `Log::*`, `perf::SetCounter`,
   and `BLAZE_PROFILE_SCOPE` are safe from any thread.
3. **Bound pointers must stay valid.** If the object dies (level unload), call `config::Unbind("key")`
   first. The same goes for `debug::Unwatch(label)`, `debug::UnregisterCommand(name)`, and `UnregisterPanel(id)`.
4. **Config keys are permanent API**: lowercase, dotted, `area.name`. Renaming one loses saved values.
   Never use the `blaze.` prefix (reserved for menu settings).
5. **Colours come from `blaze::theme::Colors()`** (`text`, `textMuted`, `accent`, `success`, `warning`,
   `error`, `surface`, `border`…). Hard-coded RGB looks wrong in light mode.
6. **Scale pixel sizes** with `blaze::GetUIScale()` (e.g. `ImVec2(0, 120 * blaze::GetUIScale())`).
7. **Order matters:** scene rendering → `NewFrame()` → `Render(rtv)` → `Present`. Calling `Render`
   before the scene is drawn blurs an empty or old frame.
8. **Release your back-buffer RTV before `ResizeBuffers`**, and wrap the call in `OnResizeBegin/End`.
9. Call `blaze::Shutdown()` **before** releasing the D3D device. It holds references and saves configs.
10. Gate gameplay input on `blaze::IsMenuOpen()` when the game uses Raw Input / DirectInput.

---

## 6. Verify the integration

Run through this before reporting the work as done:

- [ ] Project builds without new warnings.
- [ ] The game starts, the toggle key (Insert by default) opens and closes the menu, and the background blurs behind it.
- [ ] Custom pages appear in the sidebar under the right category, and their controls change the game live.
- [ ] Change a bound value, wait about 1 s, restart: the value persists. Check `%APPDATA%\<appName>\profiles\default.json`.
- [ ] Resize or alt-tab the window: no crash, and the blur still works.
- [ ] Debug → Console shows the game's log lines; `help` lists the custom commands.
- [ ] Performance → CPU scopes shows the `BLAZE_PROFILE_SCOPE` names.
- [ ] If the game still reacts to mouse/keyboard while the menu is open, apply rule 10.

**Runtime checks an agent can script** (from code, a test hook, or the console):

```cpp
blaze::debug::Execute("blaze.vars");                 // logs every bound key and value
blaze::config::SetFromJson("world.time_scale", "2"); // set any key (returns false if unknown/wrong type)
std::string v = blaze::config::GetJson("world.time_scale");
std::string state = blaze::DumpStateJson();          // perf, scopes, counters, vars, watches, recent log
blaze::debug::Execute("blaze.dump");                 // same, written to <configDir>\state_dump.json
```

---

## 7. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Menu never appears | `NewFrame`/`Render` not called each frame, or called on a different swap chain. Check `blaze::IsInitialized()`. |
| Menu draws but no blur | Blur shaders failed (see Debug console, category `blur`), Settings → Acrylic → Blur is off, or `Render` received the wrong RTV. |
| Blur shows a black/stale image | `Render` runs before the scene is drawn, or the RTV is not the back buffer. |
| Crash or `DXGI_ERROR_INVALID_CALL` on resize | The RTV wasn't released before `ResizeBuffers`. See rule 8. |
| Icons show as empty squares/blanks | Segoe Fluent Icons / MDL2 missing (very old Windows), or Mode C without `blaze::LoadFonts()`. |
| Toggle key does nothing | The game window isn't the foreground window, or a text field has focus. Try rebinding in Settings → Input. |
| Game still moves the camera with the menu open | The game uses Raw Input. See rule 10. |
| Second ImGui context assert | The project already has ImGui. Use Mode C (`manageImGui = false`). |
| Menu never appears in a DLL that compiles ImGui itself | Two copies of ImGui linked in. Use one shared `imgui` target (see "Lessons" in Mode C). |
| Player can't move while the menu is open | `blockInputWhenOpen` is true. Set it to false and route input yourself (see "Lessons" in Mode C). |
| Values don't persist | Auto-save is off (Configs page) or the variable was bound **after** a different profile loaded with another value. Bind at startup. |

---

## 8. File layout the agent should create in the target project

```
src/debug/debug_menu.h      // void RegisterDebugPages();
src/debug/debug_menu.cpp    // binds, panels, commands, watches (section 3)
docs/BLAZEIMGUI.md          // copy of this file, referenced from the project's AGENTS.md
```

Then add this line to the target project's `AGENTS.md` / `CLAUDE.md`:

> Debug UI uses BlazeImGui. Follow `docs/BLAZEIMGUI.md`. New debug pages go in `src/debug/debug_menu.cpp`.
