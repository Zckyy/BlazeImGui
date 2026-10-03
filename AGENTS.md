# AGENTS.md — BlazeImGui guide for AI coding agents

> **Integrating BlazeImGui into a game project?** Follow [docs/INTEGRATION.md](docs/INTEGRATION.md) instead —
> it is the step-by-step implementation guide. This file is for working on the library itself.

This file is the source of truth for automated agents (Claude, Codex, Copilot, Cursor…)
working **on** this library or **with** it inside a game project. Read it fully before editing.

## What this is

A drop-in Dear ImGui debug menu for **Windows + Direct3D 11** games and tools:
acrylic (Windows 11 style) background blur, dark/light themes, JSON config profiles,
Performance + Debug pages, and a panel registry for custom pages. Hosts on other renderers
(e.g. D3D12) can use it through Integration C without blur (`BLAZE_RENDERER_DX11=OFF`).

- Language: C++17, MSVC, `/W4 /permissive-`. Build must stay **warning-free**.
- Dear ImGui is pinned via CMake FetchContent (`BLAZE_IMGUI_TAG`, currently `v1.91.8`).
- Public API: `include/blaze/*.h` (umbrella header `blaze/blaze.h`). Everything in `src/` is private.

## Build & verify

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64   # or "Visual Studio 17 2022"
cmake --build build --config Release
```

Or `cmake --preset vs2026 && cmake --build --preset release` (single `build/` folder, `build/BlazeImGui.slnx`).
CI (`.github/workflows/build.yml`) builds both `BLAZE_RENDERER_DX11=ON` and `OFF` with
`BLAZE_WARNINGS_AS_ERRORS=ON`. Run both locally before pushing a change that touches `src/` or CMake.

**Visual verification without a window** (use this after any UI change):

```bash
build/Release/blaze_demo.exe --capture out.png --page blaze.settings
build/Release/blaze_demo.exe --capture light.png --page game.world --exec "blaze.theme light"
build/Release/blaze_demo.exe --capture closed.png --exec "blaze.toggle" --exec "blaze.set blaze.overlay.enabled true"
```

`--capture` renders a hidden window, waits `--delay` seconds (default 1.5) for animations to settle,
writes the back buffer as PNG and exits (code 0 on success). It uses a throw-away config dir
(`%TEMP%\BlazeDemoCapture`) unless `--config-dir` is given. Delete that dir between runs for a clean state.
Other flags: `--page <panel id>`, `--exec "<console command>"` (repeatable), `--size 1920x1080`.

**Interaction checks:** `--drag x,y,dx,dy` injects a left-button drag (client pixels, starting 0.5 s
in) and `--dump state.json` writes `DumpStateJson()` at capture time. It includes `main_window`
`{x,y,w,h}`, so a drag can be asserted numerically:

```bash
build/Release/blaze_demo.exe --capture d.png --dump d.json --drag 900,140,300,75 --delay 2
# main_window moves from (280,110) to (580,185) at the default 1600x900 size
```

Built-in panel ids: `blaze.performance`, `blaze.debug`, `blaze.configs`, `blaze.settings`.
Demo panel ids: `game.player`, `game.world`, `game.entities`.

## Repository map

| Path | Purpose |
|---|---|
| `include/blaze/blaze.h` | Lifecycle (`Initialize/NewFrame/Render/Shutdown`), panels, toasts, blur/overlay settings |
| `include/blaze/widgets.h` | Styled widgets (`ui::Toggle`, `ui::SliderFloat`, `ui::BeginCard`…) |
| `include/blaze/theme.h` | Semantic palette (`theme::Colors()`), modes, accents |
| `include/blaze/config.h` | Variable binding + JSON profiles |
| `include/blaze/perf.h` | Frame stats, `BLAZE_PROFILE_SCOPE`, counters |
| `include/blaze/debug.h` | `Log::*`, console commands, watches |
| `include/blaze/icons.h` | Segoe Fluent icon codepoints (`icons::Bug` etc.) |
| `src/blaze.cpp` | Core: init, input, fonts, main window layout, overlay, toasts, core commands |
| `src/blur_dx11.cpp` | Acrylic blur (dual-Kawase + composite). Saves/restores all D3D11 state |
| `src/blur_none.cpp` | Blur stub used when `BLAZE_RENDERER_DX11=OFF` |
| `src/widgets.cpp` / `src/theme.cpp` | Widget drawing / palettes |
| `src/config.cpp` + `src/json.*` | Profiles and the tiny JSON parser/writer |
| `src/panels/panel_*.cpp` | Built-in pages (one file each) |
| `examples/dx11_demo/` | Reference integration + `--capture` tool |
| `templates/panel_template.cpp` | Copy-paste starting point for a new page |

## How to add a page to a game (most common task)

1. Copy `templates/panel_template.cpp` into the game project.
2. Bind persistent variables with `blaze::config::Bind("area.name", &var)` **once**, at startup, after `blaze::Initialize`.
3. Register a `blaze::Panel` with a unique stable `id` (`"game.<thing>"`), a `category`
   (`"Game"`, `"Debug"`, `"Tools"`… — categories `Overview, Game, Debug, Tools, System` are pre-ordered), an `icon`, and a `draw` lambda.
4. Inside `draw`, use `ui::BeginCard(...)`/`ui::EndCard()` around groups of setting rows.

```cpp
blaze::config::Bind("render.wireframe", &g_wireframe);
blaze::RegisterPanel({
    .id = "game.render", .title = "Rendering", .subtitle = "Debug views",
    .category = "Game", .icon = blaze::icons::Monitor, .order = 30,
    .draw = [] {
        using namespace blaze::ui;
        if (BeginCard("Views", blaze::icons::View)) {
            Toggle("Wireframe", &g_wireframe, "Draw all meshes as lines");
        }
        EndCard();
    }});
```
(Designated initializers need C++20; in C++17 assign fields one by one as the demo does.)

## Conventions & invariants (do not break)

- **`EndCard()` is always called**, even when `BeginCard()` returns false. Cards cannot be nested.
- Config keys: lowercase, dotted, stable (`player.god_mode`). Renaming a key orphans saved values.
  Global menu settings live under `blaze.*` in `blaze_settings.json`; never bind game vars to `blaze.*`.
- Bound pointers must outlive the binding. Call `config::Unbind(key)` before destroying the variable.
- Colors: read from `theme::Colors()` (semantic: `text`, `textMuted`, `accent`, `surface`, `border`,
  `success/warning/error/info`…). Never hard-code RGB in panels — it breaks light mode.
- Sizes: multiply literal pixel sizes by `blaze::GetUIScale()` (widgets already do this).
- Threading: `Log::*`, `perf::SetCounter`, `BLAZE_PROFILE_SCOPE` are thread-safe. Everything else
  (panels, config, commands, watches) is **render-thread only**.
- `blur_dx11.cpp` must leave the D3D11 pipeline exactly as it found it (hooked-Present safety).
  If you touch new state there, add it to `StateBackup`.
- Fonts come from `C:\Windows\Fonts` (Segoe UI, Segoe UI Semibold, Consolas, Segoe Fluent Icons /
  MDL2). Code must keep working if any are missing (`GetFonts().iconsLoaded` guards icon glyphs).
- No new third-party dependencies without a strong reason; the library is intentionally self-contained.
- Keep `/W4` warning-free; cast int→float explicitly in `S()`-style helpers.
- Code that drags or positions the main window must use `g.mainPos` (set in `DrawMainWindow`),
  never `ImGui::GetWindowPos()` from inside a child window. Child positions are offset from the
  main window, which once made header drags jump by the sidebar width every frame.
- Anything only Integration A/B needs (backend calls, D3D11 types) goes behind `#ifndef BLAZE_NO_DX11`.
  Integration C hosts get explicit hooks instead (`ConsumeFontRebuild()`, `GetToggleKey()`).

## Runtime introspection (for agents driving a running game)

Console commands (Debug page, or `blaze::debug::Execute("...")` from code):

| Command | Effect |
|---|---|
| `help` | List all commands |
| `blaze.vars` | List every bound config variable and value |
| `blaze.set <key> <json>` | Set any variable, e.g. `blaze.set world.time_scale 2.5`, `blaze.set world.grid_color [1,0,0,1]` |
| `blaze.dump [file]` | Write JSON snapshot (perf stats, scopes, counters, vars, watches, recent log) — default `<configDir>/state_dump.json` |
| `blaze.profile list\|load\|save [name]` | Manage profiles |
| `blaze.theme dark\|light\|system`, `blaze.blur on\|off\|1-6`, `blaze.scale 1.25`, `blaze.toggle` | Menu settings |

From code: `blaze::DumpStateJson()`, `blaze::config::SetFromJson(key, json)`, `blaze::config::GetJson(key)`.

## Config files

`%APPDATA%\<appName>\` by default (override with `InitInfo::configDir`):

```
blaze_settings.json        global menu settings (theme, blur, hotkey, overlay, last profile)
profiles/<name>.json       one file per profile: { "blaze_config_version": 1, "profile": "...", "values": { key: value } }
imgui_layout.ini           ImGui window positions
```
Values: bool → `true`, int/float → number, string → `"..."`, color → `[r,g,b,a]` (0..1), key → Win32 VK code.
Unknown keys in a file are preserved on save, so hand-edited or not-yet-registered values survive.
Files are written atomically (temp file + rename). `//` line comments are tolerated when reading.

## Integration modes (see `blaze.h` header comment for code)

- **A — own render loop**: `Initialize` → per frame `NewFrame()` then `Render(rtv)` → `Present`.
- **B — hooked Present**: same calls inside the hook; create/cache an RTV for the back buffer and call
  `OnResizeBegin/End` around `ResizeBuffers`. Forward the game's WndProc to `blaze::WndProcHandler`
  (return early when it returns true).
- **C on another renderer** (D3D12…): build with `BLAZE_RENDERER_DX11=OFF`, leave `device`/`context` null,
  call `DrawUI()` only. Everything compiled under `BLAZE_NO_DX11` must keep building: guard any new
  D3D11 code with `#ifndef BLAZE_NO_DX11` or keep it in `blur_dx11.cpp`.
- **C — existing ImGui**: `manageImGui = false`; call `DrawUI()` inside your frame and `RenderBlur(rtv)`
  before `ImGui_ImplDX11_RenderDrawData`.

## Checklist before finishing a change

- [ ] `cmake --build build --config Release` passes with **zero warnings**.
- [ ] Captured the affected page(s) in **both** dark and light themes with `--capture` and looked at them.
- [ ] New public API is documented in its header with a usage example.
- [ ] New persistent settings use `config::Bind` (not ad-hoc files).
- [ ] Updated this file if conventions, commands or layout changed.
- [ ] Both renderer configs build (`-DBLAZE_RENDERER_DX11=OFF` too) when `src/` or CMake changed.
- [ ] Interactive changes (drag, sliders, clicks) checked with `--drag` + `--dump`.
- [ ] `CHANGELOG.md` updated for user-visible changes.
