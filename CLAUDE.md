# CLAUDE.md

All agent guidance for this repository lives in [AGENTS.md](AGENTS.md) — read it first.
To integrate BlazeImGui into another project, follow [docs/INTEGRATION.md](docs/INTEGRATION.md).

Quick reference:
- Build: `cmake -S . -B build -G "Visual Studio 18 2026" -A x64 && cmake --build build --config Release`
- Verify UI changes headlessly: `build/Release/blaze_demo.exe --capture out.png --page <panel id> [--exec "blaze.theme light"]`, then view the PNG.
- Build must be warning-free (`/W4`). Panels use `theme::Colors()`, never hard-coded colors.
