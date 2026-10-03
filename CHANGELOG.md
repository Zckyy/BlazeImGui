# Changelog

## 1.1.0

### Added
- Renderer-agnostic build: `-DBLAZE_RENDERER_DX11=OFF` removes all D3D11 code and the
  `d3d11` / `d3dcompiler` dependencies. Integration C works on any ImGui backend (for example
  D3D12). `InitInfo::device` / `context` are optional when `manageImGui = false`.
- `InitInfo::adapter`: GPU name and VRAM on the Performance page when there is no D3D11 device.
- `blaze::BlurAvailable()`, `blaze::GetToggleKey()`, `blaze::ConsumeFontRebuild()` (font rebuilds
  after interface-scale changes in Integration C).
- `DumpStateJson()` includes `main_window` `{x, y, w, h}`.
- Demo: `--drag x,y,dx,dy` input injection and `--dump state.json` for headless interaction checks.
- `CMakePresets.json` (single `build/` folder), GitHub Actions CI for both renderer configs,
  and the `BLAZE_WARNINGS_AS_ERRORS` option.
- INTEGRATION.md: Mode C on D3D12, and lessons from a hooked-Present D3D12 integration.

### Changed
- The main window background is near-opaque when blur is off or unavailable, so text stays
  readable over busy game scenes.
- The panel template is compile-checked in both renderer configurations.

### Fixed
- Dragging the menu by its header made the window jump right by the sidebar width every frame.
- `_wfopen` deprecation warning (C4996) in `json.cpp` when built by other projects.

## 1.0.0

- Initial release.
