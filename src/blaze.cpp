// =============================================================================
// BlazeImGui core: lifecycle, input, fonts, main window layout, toasts, overlay.
// =============================================================================
#include "internal.h"
#include "json.h"

#include <windows.h>
#include <dxgi.h>
#include <imgui_impl_win32.h>
#ifndef BLAZE_NO_DX11
#include <d3d11.h>
#include <imgui_impl_dx11.h>
#endif

#include <algorithm>
#include <cmath>
#include <unordered_map>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace blaze {
namespace {

using theme::U32;

struct Toast {
    std::string msg;
    ToastKind   kind;
    float       life, total;
    float       anim = 0;
};

struct State {
    bool                 initialized = false;
    InitInfo             info;
    ID3D11Device*        device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    HWND                 hwnd = nullptr;
    std::string          appName;
    std::string          iniPath;

    bool   menuOpen = true;
    float  visibility = 0;
    int    prevToggleVk = 0;
    bool   prevToggleDown = false;
    bool   fontsDirty = false;
    float  appliedScale = 0;
    int    appliedMode = -1;
    ImVec4 appliedAccent{ -1, -1, -1, -1 };

    std::vector<Panel>       panels;
    std::vector<std::string> categoryOrder{ "Overview", "Game", "Debug", "Tools", "System" };
    std::string              selected;
    std::string              searchBuf;
    std::string              searchLower;
    float                    pageAnim = 1;
    ImVec2                   mainPos;     // Main window position this frame (after clamping)
    ImVec2                   mainSize;

    std::vector<Toast> toasts;
    BlurSettings       blur;
    OverlaySettings    overlay;
    detail::CoreSettings settings;
    detail::Fonts      fonts;
    std::unordered_map<ImGuiID, float> anims;
    LARGE_INTEGER      startTime{}, freq{};
};
State g;

inline float S(float v) { return v * g.settings.uiScale; }
inline const theme::Palette& C() { return theme::Colors(); }

std::string Lower(std::string s) {
    for (auto& ch : s) ch = char(tolower((unsigned char)ch));
    return s;
}

std::string DefaultConfigDir(const char* appName) {
    wchar_t* appdata = nullptr;
    size_t len = 0;
    std::wstring base = L".";
    if (_wdupenv_s(&appdata, &len, L"APPDATA") == 0 && appdata) { base = appdata; free(appdata); }
    std::string name = appName && *appName ? appName : "BlazeImGui";
    for (auto& ch : name) if (strchr("\\/:*?\"<>|", ch)) ch = '_';
    return json::Narrow(base) + "\\" + name;
}

// -----------------------------------------------------------------------------
// Fonts
// -----------------------------------------------------------------------------
std::string FontPath(const char* file) {
    wchar_t windir[MAX_PATH];
    UINT n = GetWindowsDirectoryW(windir, MAX_PATH);
    std::string dir = n ? json::Narrow(windir) : "C:\\Windows";
    return dir + "\\Fonts\\" + file;
}

bool FileExists(const std::string& utf8) {
    return GetFileAttributesW(json::Widen(utf8).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// -----------------------------------------------------------------------------
// Sorting / lookup
// -----------------------------------------------------------------------------
int CategoryRank(const std::string& cat) {
    for (size_t i = 0; i < g.categoryOrder.size(); ++i)
        if (g.categoryOrder[i] == cat) return int(i);
    return 1000;
}

void SortPanels() {
    std::stable_sort(g.panels.begin(), g.panels.end(), [](const Panel& a, const Panel& b) {
        int ra = CategoryRank(a.category), rb = CategoryRank(b.category);
        if (ra != rb) return ra < rb;
        if (a.category != b.category) return a.category < b.category;
        if (a.order != b.order) return a.order < b.order;
        return a.title < b.title;
    });
}

Panel* FindPanel(const std::string& id) {
    for (auto& p : g.panels) if (p.id == id) return &p;
    return nullptr;
}

// -----------------------------------------------------------------------------
// Input
// -----------------------------------------------------------------------------
void PollToggleKey() {
    int vk = g.settings.toggleKey;
    bool down = vk > 0 && (GetAsyncKeyState(vk) & 0x8000) != 0;
    if (vk != g.prevToggleVk) { g.prevToggleVk = vk; g.prevToggleDown = down; return; }
    // Only react when our window is focused (avoids toggling from other apps).
    bool focused = !g.hwnd || GetForegroundWindow() == g.hwnd ||
                   GetAncestor(GetForegroundWindow(), GA_ROOTOWNER) == g.hwnd;
    if (down && !g.prevToggleDown && focused && !ImGui::GetIO().WantTextInput) ToggleMenu();
    g.prevToggleDown = down;
}

// -----------------------------------------------------------------------------
// Drawing helpers
// -----------------------------------------------------------------------------
// Drags the main window by the last item (an InvisibleButton handle). The handles
// live in child windows, so the base must be the main window's position, not
// ImGui::GetWindowPos() (which would add the child's offset every frame).
void DragMainWindowByItem() {
    if (!ImGui::IsItemActive() || !ImGui::IsMouseDragging(0, 0.0f)) return;
    g.mainPos += ImGui::GetIO().MouseDelta;
    ImGui::SetWindowPos("###BlazeMain", g.mainPos);
}

void DrawShadow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float alpha) {
    const int layers = 8;
    for (int i = layers; i >= 1; --i) {
        float spread = S(2.5f) * float(i);
        float k = (1.0f - float(i) / float(layers + 1));
        dl->AddRectFilled(ImVec2(a.x - spread, a.y - spread + S(6)), ImVec2(b.x + spread, b.y + spread + S(6)),
                          U32(C().shadow, alpha * k * k * 0.22f), rounding + spread);
    }
}

const char* ToastIcon(ToastKind k) {
    switch (k) {
    case ToastKind::Success: return icons::Checkmark;
    case ToastKind::Warning: return icons::Warning;
    case ToastKind::Error:   return icons::Error;
    default:                 return icons::Info;
    }
}

ImVec4 ToastColor(ToastKind k) {
    switch (k) {
    case ToastKind::Success: return C().success;
    case ToastKind::Warning: return C().warning;
    case ToastKind::Error:   return C().error;
    default:                 return C().accent;
    }
}

bool NavItem(const Panel& p, bool selected, float* outCenterY) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = S(36);
    ImGui::PushID(p.id.c_str());
    bool pressed = ImGui::InvisibleButton("##nav", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    float hv = detail::Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 20.0f);
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(pos.x + w, pos.y + h);
    if (selected) dl->AddRectFilled(pos, b, U32(C().surfaceActive), S(7));
    else if (hv > 0.01f) dl->AddRectFilled(pos, b, U32(C().surfaceHover, hv), S(7));

    float lh = ImGui::GetTextLineHeight();
    float x = pos.x + S(14);
    if (p.icon && *p.icon && g.fonts.iconsLoaded) {
        dl->AddText(ImVec2(x, pos.y + (h - lh) * 0.5f), U32(selected ? C().accent : theme::Lerp(C().textMuted, C().text, hv)), p.icon);
        x += S(28);
    }
    ImFont* f = selected ? ui::GetFont(ui::Font::Semibold) : ui::GetFont(ui::Font::Regular);
    dl->AddText(f, f->FontSize, ImVec2(x, pos.y + (h - lh) * 0.5f),
                U32(selected ? C().text : theme::Lerp(C().textMuted, C().text, hv)), p.title.c_str());
    if (outCenterY) *outCenterY = pos.y + h * 0.5f;
    return pressed;
}

void DrawSidebar(float width, float height) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(12), S(14)));
    ImGui::BeginChild("##sidebar", ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // ---- Brand block (also a drag handle) ----
    ImVec2 p = ImGui::GetCursorScreenPos();
    float bw = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##drag_brand", ImVec2(bw, S(44)));
    DragMainWindowByItem();
    float logo = S(34);
    ImVec2 la(p.x + S(4), p.y + S(5)), lb(la.x + logo, la.y + logo);
    dl->AddRectFilled(la, lb, U32(C().accent), S(9));
    dl->AddRectFilled(la, ImVec2(lb.x, la.y + logo * 0.5f), U32(ImVec4(1, 1, 1, 0.14f)), S(9), ImDrawFlags_RoundCornersTop);
    const char* glyph = g.fonts.iconsLoaded ? icons::Flash.c_str() : "B";
    ImVec2 gs = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(la.x + (logo - gs.x) * 0.5f, la.y + (logo - gs.y) * 0.5f), U32(C().accentText), glyph);
    ImFont* sb = ui::GetFont(ui::Font::Semibold);
    dl->AddText(sb, sb->FontSize, ImVec2(lb.x + S(10), la.y + S(0)), U32(C().text), g.appName.c_str());
    char sub[64]; snprintf(sub, sizeof sub, "Debug Menu  v%s", kVersion);
    dl->AddText(ImVec2(lb.x + S(10), la.y + S(17)), U32(C().textMuted), sub);

    ImGui::Dummy(ImVec2(0, S(6)));

    // ---- Search ----
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(32), S(7)));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(7));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, C().border);
        ImVec2 sp = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemWidth(-FLT_MIN);
        char buf[128];
        strncpy_s(buf, g.searchBuf.c_str(), _TRUNCATE);
        if (ImGui::InputTextWithHint("##search", "Search pages", buf, sizeof buf)) {
            g.searchBuf = buf;
            g.searchLower = Lower(g.searchBuf);
        }
        float fh = ImGui::GetFrameHeight();
        if (g.fonts.iconsLoaded)
            dl->AddText(ImVec2(sp.x + S(10), sp.y + (fh - ImGui::GetTextLineHeight()) * 0.5f), U32(C().textMuted), icons::Search);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);
    }
    ImGui::Dummy(ImVec2(0, S(4)));

    // ---- Navigation list (scrolls independently) ----
    float footerH = S(52);
    ImGui::BeginChild("##navlist", ImVec2(0, ImGui::GetContentRegionAvail().y - footerH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    std::string lastCat = "\x01";
    float selY = -1;
    ImDrawList* ndl = ImGui::GetWindowDrawList();
    for (auto& panel : g.panels) {
        if (!g.searchLower.empty() && Lower(panel.title).find(g.searchLower) == std::string::npos &&
            Lower(panel.category).find(g.searchLower) == std::string::npos)
            continue;
        const std::string& cat = panel.category.empty() ? std::string("General") : panel.category;
        if (cat != lastCat) {
            lastCat = cat;
            ImGui::Dummy(ImVec2(0, S(6)));
            ImVec2 cp = ImGui::GetCursorScreenPos();
            std::string upper = cat;
            for (auto& ch : upper) ch = char(toupper((unsigned char)ch));
            ImFont* f = ui::GetFont(ui::Font::Semibold);
            ndl->AddText(f, f->FontSize * 0.78f, ImVec2(cp.x + S(14), cp.y), U32(C().textDisabled), upper.c_str());
            ImGui::Dummy(ImVec2(0, f->FontSize * 0.78f + S(2)));
        }
        float cy = 0;
        bool sel = panel.id == g.selected;
        if (NavItem(panel, sel, &cy)) SelectPanel(panel.id);
        if (sel) selY = cy;
    }
    // Animated selection indicator
    if (selY >= 0) {
        float y = detail::Animate(ImGui::GetID("##indicator"), selY, 16.0f);
        float ih = S(16);
        ImVec2 wp = ImGui::GetWindowPos();
        ndl->AddRectFilled(ImVec2(wp.x + S(0), y - ih * 0.5f), ImVec2(wp.x + S(3), y + ih * 0.5f), U32(C().accent), S(2));
    }
    ImGui::EndChild();

    // ---- Footer: active profile chip ----
    ImVec2 fp = ImGui::GetCursorScreenPos();
    float fw = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(0, S(6)));
    ImGui::SetCursorScreenPos(ImVec2(fp.x, fp.y + S(8)));
    bool clicked = ImGui::InvisibleButton("##profilechip", ImVec2(fw, S(38)));
    bool hov = ImGui::IsItemHovered();
    ImVec2 ca = ImGui::GetItemRectMin(), cb = ImGui::GetItemRectMax();
    dl->AddRectFilled(ca, cb, U32(hov ? C().surfaceHover : C().surface), S(8));
    dl->AddRect(ca, cb, U32(C().border), S(8));
    float lh = ImGui::GetTextLineHeight();
    float tx = ca.x + S(12);
    if (g.fonts.iconsLoaded) {
        dl->AddText(ImVec2(tx, ca.y + (S(38) - lh) * 0.5f), U32(C().textMuted), icons::Document);
        tx += S(24);
    }
    dl->AddText(ImVec2(tx, ca.y + (S(38) - lh) * 0.5f), U32(C().text), config::ActiveProfile().c_str());
    if (config::IsDirty()) {
        dl->AddCircleFilled(ImVec2(cb.x - S(14), (ca.y + cb.y) * 0.5f), S(3.5f), U32(C().warning), 12);
        if (hov) ImGui::SetTooltip("Unsaved changes in profile '%s'", config::ActiveProfile().c_str());
    } else if (hov) ImGui::SetTooltip("Active config profile - click to manage");
    if (clicked) SelectPanel("blaze.configs");

    ImGui::EndChild();
}

void DrawHeader(const Panel* panel, float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = S(68);
    float btn = ImGui::GetFrameHeight() + S(4);
    float rightW = btn * 3 + S(8) * 3 + S(96);

    // Drag region (everything except the buttons on the right)
    ImGui::InvisibleButton("##drag_header", ImVec2(std::max(1.0f, width - rightW - S(24)), h));
    DragMainWindowByItem();

    float x = p.x + S(28);
    if (panel) {
        ImFont* tf = ui::GetFont(ui::Font::Title);
        float ty = p.y + S(panel->subtitle.empty() ? 20.0f : 13.0f);
        dl->AddText(tf, tf->FontSize, ImVec2(x, ty), U32(C().text), panel->title.c_str());
        if (!panel->subtitle.empty())
            dl->AddText(ImVec2(x, ty + tf->FontSize + S(1)), U32(C().textMuted), panel->subtitle.c_str());
    }

    // Right cluster: FPS chip, theme, overlay, close
    float bx = p.x + width - S(20) - btn;
    float by = p.y + (h - btn) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    // Close button with red hover
    {
        ImVec2 a = ImGui::GetCursorScreenPos();
        bool pressed = ImGui::InvisibleButton("##close", ImVec2(btn, btn));
        bool hov = ImGui::IsItemHovered();
        float hv = detail::Animate(ImGui::GetItemID(), hov ? 1.0f : 0.0f, 20.0f);
        dl->AddRectFilled(a, ImVec2(a.x + btn, a.y + btn), U32(ImVec4(0.90f, 0.20f, 0.22f, 1.0f), hv), S(6));
        const char* gl = g.fonts.iconsLoaded ? icons::Close.c_str() : "x";
        ImVec2 ts = ImGui::CalcTextSize(gl);
        dl->AddText(ImVec2(a.x + (btn - ts.x) * 0.5f, a.y + (btn - ts.y) * 0.5f),
                    U32(theme::Lerp(C().textMuted, ImVec4(1, 1, 1, 1), hv)), gl);
        if (hov) ImGui::SetTooltip("Close menu (%s)", ui::VirtualKeyName(g.settings.toggleKey));
        if (pressed) SetMenuOpen(false);
    }
    bx -= btn + S(6);
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    if (ui::IconButton(icons::Speed, g.overlay.enabled ? "Hide performance overlay" : "Show performance overlay", g.overlay.enabled))
        g.overlay.enabled = !g.overlay.enabled;
    bx -= btn + S(6);
    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    bool dark = theme::IsDark();
    if (ui::IconButton(dark ? icons::Sun : icons::Moon, dark ? "Switch to light mode" : "Switch to dark mode")) {
        g.settings.themeMode = int(dark ? theme::Mode::Light : theme::Mode::Dark);
        detail::ApplyThemeSettings();
    }

    if (g.settings.showFpsInHeader) {
        const auto& fs = perf::Frame();
        char fps[32]; snprintf(fps, sizeof fps, "%.0f FPS", fs.fps);
        ImVec2 ts = ImGui::CalcTextSize(fps);
        float cw = ts.x + S(30), ch = btn - S(6);
        ImVec2 a(bx - S(10) - cw, p.y + (h - ch) * 0.5f);
        float target = perf::GetTargetFps();
        ImVec4 col = fs.fps >= target * 0.95f ? C().success : fs.fps >= target * 0.5f ? C().warning : C().error;
        dl->AddRectFilled(a, ImVec2(a.x + cw, a.y + ch), U32(col, 0.13f), ch * 0.5f);
        dl->AddCircleFilled(ImVec2(a.x + S(12), a.y + ch * 0.5f), S(3.5f), U32(col), 12);
        dl->AddText(ImVec2(a.x + S(21), a.y + (ch - ts.y) * 0.5f), U32(col), fps);
    }

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    dl->AddLine(ImVec2(p.x, p.y + h - 1), ImVec2(p.x + width, p.y + h - 1), U32(C().border));
}

void DrawStatusBar(float width) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float h = S(30);
    ImGui::Dummy(ImVec2(width, h));
    dl->AddLine(p, ImVec2(p.x + width, p.y), U32(C().border));
    float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;

    char left[160];
    snprintf(left, sizeof left, "Profile: %s%s", config::ActiveProfile().c_str(), config::IsDirty() ? "  (unsaved)" : "");
    dl->AddText(ImVec2(p.x + S(28), ty), U32(C().textMuted), left);

    const auto& fs = perf::Frame();
    char right[160];
    snprintf(right, sizeof right, "%.2f ms   |   %s to toggle", fs.frameMs, ui::VirtualKeyName(g.settings.toggleKey));
    ImVec2 ts = ImGui::CalcTextSize(right);
    dl->AddText(ImVec2(p.x + width - ts.x - S(24), ty), U32(C().textMuted), right);
}

void DrawMainWindow() {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(S(1040), S(680)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(780), S(500)), ImVec2(FLT_MAX, FLT_MAX));

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g.visibility);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(12));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(8)));
    ImGui::PushStyleColor(ImGuiCol_Border, C().border);
    // The translucent palette background is tuned for acrylic blur. Without blur the
    // game would show through sharply behind the text, so go (almost) opaque instead.
    ImVec4 bg = C().background;
    if (!g.blur.enabled || !detail::blur::Available()) bg.w = std::max(bg.w, 0.97f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, bg);

    bool visible = ImGui::Begin("###BlazeMain", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar(1); // WindowPadding stays 0 for children below; Alpha stays pushed
    if (visible) {
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();

        // Keep at least part of the window on screen.
        ImVec2 clamped(std::clamp(wp.x, -ws.x + S(120), io.DisplaySize.x - S(120)),
                       std::clamp(wp.y, 0.0f, io.DisplaySize.y - S(60)));
        if (clamped.x != wp.x || clamped.y != wp.y) ImGui::SetWindowPos(clamped);
        g.mainPos = clamped;
        g.mainSize = ws;

        DrawShadow(ImGui::GetBackgroundDrawList(), wp, wp + ws, S(12), g.visibility);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        float sbW = S(232);
        dl->AddRectFilled(wp, ImVec2(wp.x + sbW, wp.y + ws.y), U32(C().sidebar), S(12), ImDrawFlags_RoundCornersLeft);
        dl->AddLine(ImVec2(wp.x + sbW, wp.y), ImVec2(wp.x + sbW, wp.y + ws.y), U32(C().border));

        ImGui::SetCursorPos(ImVec2(0, 0));
        DrawSidebar(sbW, ws.y);

        Panel* panel = FindPanel(g.selected);
        if (!panel && !g.panels.empty()) { g.selected = g.panels.front().id; panel = &g.panels.front(); }

        ImGui::SetCursorPos(ImVec2(sbW, 0));
        float mainW = ws.x - sbW;
        ImGui::BeginChild("##main", ImVec2(mainW, ws.y), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        DrawHeader(panel, mainW);

        float statusH = S(30);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28), S(18)));
        ImGui::BeginChild("##page", ImVec2(mainW, ImGui::GetContentRegionAvail().y - statusH),
                          ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
        ImGui::PopStyleVar();
        g.pageAnim = std::min(1.0f, g.pageAnim + io.DeltaTime * 6.0f);
        float ease = 1.0f - (1.0f - g.pageAnim) * (1.0f - g.pageAnim);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g.visibility * ease);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (1.0f - ease) * S(10));
        if (panel && panel->draw) {
            ImGui::PushID(panel->id.c_str());
            panel->draw();
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, S(8)));
        ImGui::PopStyleVar();
        ImGui::EndChild();

        DrawStatusBar(mainW);
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
}

void DrawOverlay() {
    if (!g.overlay.enabled) return;
    ImGuiIO& io = ImGui::GetIO();
    const float pad = S(14);
    int corner = std::clamp(g.overlay.corner, 0, 3);
    ImVec2 pos((corner & 1) ? io.DisplaySize.x - pad : pad, (corner & 2) ? io.DisplaySize.y - pad : pad);
    ImVec2 pivot((corner & 1) ? 1.0f : 0.0f, (corner & 2) ? 1.0f : 0.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(14), S(10)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::Begin("##BlazeOverlay", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
        bool dark = theme::IsDark();
        dl->AddRectFilled(wp, wp + ws, U32(dark ? ImVec4(0.08f, 0.08f, 0.10f, 0.82f) : ImVec4(1, 1, 1, 0.88f)), S(10));
        dl->AddRect(wp, wp + ws, U32(C().border), S(10));

        const auto& fs = perf::Frame();
        float target = perf::GetTargetFps();
        ImVec4 col = fs.fps >= target * 0.95f ? C().success : fs.fps >= target * 0.5f ? C().warning : C().error;
        ui::PushFont(ui::Font::Large);
        ImGui::TextColored(col, "%.0f", fs.fps);
        ui::PopFont();
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::TextColored(C().textMuted, "FPS");
        ImGui::TextColored(C().text, "%.2f ms", fs.frameMs);
        ImGui::EndGroup();
        ImGui::SameLine(0, S(16));
        ImGui::BeginGroup();
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::TextColored(C().textMuted, "1%% low");
        ImGui::TextColored(C().text, "%.0f", fs.low1PctFps);
        ImGui::EndGroup();
        if (g.overlay.showGraph) {
            float mx = std::max(fs.maxMs * 1.15f, 1000.0f / target * 1.5f);
            ui::Sparkline("##ovspark", fs.history + perf::kHistory - 120, 120, 0, mx, ImVec2(S(200), S(34)), col);
        }
        if (g.overlay.showSystem) {
            const auto& ss = perf::System();
            ImGui::TextColored(C().textMuted, "CPU %.0f%%   RAM %.0f MB", ss.cpuProcessPct, ss.workingSetMB);
            if (ss.vramBudgetMB > 0) ImGui::TextColored(C().textMuted, "VRAM %.0f / %.0f MB", ss.vramUsedMB, ss.vramBudgetMB);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void DrawToasts() {
    if (g.toasts.empty()) return;
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    float y = io.DisplaySize.y - S(20);
    const float w = S(320);
    for (int i = int(g.toasts.size()) - 1; i >= 0; --i) {
        Toast& t = g.toasts[size_t(i)];
        t.life -= io.DeltaTime;
        float target = t.life > 0 ? 1.0f : 0.0f;
        t.anim += (target - t.anim) * std::min(1.0f, io.DeltaTime * 12.0f);
        if (t.life <= 0 && t.anim < 0.02f) { g.toasts.erase(g.toasts.begin() + i); continue; }

        ImVec4 col = ToastColor(t.kind);
        float textW = w - S(56);
        ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, textW, t.msg.c_str());
        float h = std::max(S(48), ts.y + S(26));
        float x = io.DisplaySize.x - S(20) - w + (1.0f - t.anim) * S(60);
        ImVec2 a(x, y - h), b(x + w, y);
        float al = t.anim;
        bool dark = theme::IsDark();
        DrawShadow(dl, a, b, S(10), al * 0.6f);
        dl->AddRectFilled(a, b, U32(dark ? ImVec4(0.15f, 0.15f, 0.17f, 0.97f) : ImVec4(1, 1, 1, 0.98f), al), S(10));
        dl->AddRect(a, b, U32(C().border, al), S(10));
        dl->AddRectFilled(ImVec2(a.x, a.y + S(10)), ImVec2(a.x + S(3), b.y - S(10)), U32(col, al), S(2));
        if (g.fonts.iconsLoaded)
            dl->AddText(ImVec2(a.x + S(18), a.y + (h - ImGui::GetTextLineHeight()) * 0.5f), U32(col, al), ToastIcon(t.kind));
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + S(44), a.y + (h - ts.y) * 0.5f),
                    U32(C().text, al), t.msg.c_str(), nullptr, textW);
        // Remaining-time line
        float frac = std::clamp(t.life / t.total, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(a.x + S(10), b.y - S(3)), ImVec2(a.x + S(10) + (w - S(20)) * frac, b.y - S(1)), U32(col, al * 0.5f), S(1));
        y -= (h + S(10)) * t.anim;
    }
}

void RegisterCoreCommands() {
    using debug::Args;
    debug::RegisterCommand("blaze.toggle", "Toggle the menu", [](const Args&) { ToggleMenu(); });
    debug::RegisterCommand("blaze.theme", "blaze.theme <dark|light|system>", [](const Args& a) {
        if (a.size() < 2) { Log::Info("theme: %s", theme::IsDark() ? "dark" : "light"); return; }
        std::string m = Lower(a[1]);
        g.settings.themeMode = m == "light" ? 1 : m == "system" ? 2 : 0;
        detail::ApplyThemeSettings();
    });
    debug::RegisterCommand("blaze.blur", "blaze.blur <on|off|1-6>", [](const Args& a) {
        if (a.size() < 2) { Log::Info("blur: %s strength %d", g.blur.enabled ? "on" : "off", g.blur.strength); return; }
        if (a[1] == "on") g.blur.enabled = true;
        else if (a[1] == "off") g.blur.enabled = false;
        else { g.blur.enabled = true; g.blur.strength = std::clamp(atoi(a[1].c_str()), 1, 6); }
    });
    debug::RegisterCommand("blaze.scale", "blaze.scale <0.75-2.0> - UI scale", [](const Args& a) {
        if (a.size() >= 2) SetUIScale(float(atof(a[1].c_str())));
        Log::Info("UI scale: %.2f", GetUIScale());
    });
    debug::RegisterCommand("blaze.profile", "blaze.profile <list|load|save> [name]", [](const Args& a) {
        std::string sub = a.size() > 1 ? Lower(a[1]) : "list";
        if (sub == "load" && a.size() > 2) config::LoadProfile(a[2]);
        else if (sub == "save") config::SaveProfile(a.size() > 2 ? a[2] : std::string());
        else for (auto& p : config::ListProfiles()) Log::Info("%s%s", p.c_str(), p == config::ActiveProfile() ? "  (active)" : "");
    });
    debug::RegisterCommand("blaze.vars", "List all bound config variables", [](const Args&) {
        for (auto& v : config::ListVars())
            Log::Info("%-32s = %s%s", v.key.c_str(), v.valueJson.c_str(), v.scope == config::Scope::Global ? "  [global]" : "");
    });
    debug::RegisterCommand("blaze.set", "blaze.set <key> <json value> - set a config variable", [](const Args& a) {
        if (a.size() < 3) { Log::Error("usage: blaze.set <key> <value>"); return; }
        std::string valueText = a[2];
        for (size_t i = 3; i < a.size(); ++i) valueText += " " + a[i];
        json::Value probe;
        if (!json::Parse(valueText, probe)) valueText = "\"" + json::Escape(valueText) + "\""; // bare word -> string
        if (config::SetFromJson(a[1], valueText)) Log::Info("%s = %s", a[1].c_str(), config::GetJson(a[1]).c_str());
        else Log::Error("Unknown key or wrong type: %s (see blaze.vars)", a[1].c_str());
    });
    debug::RegisterCommand("blaze.dump", "blaze.dump [file] - write state JSON (perf, vars, log)", [](const Args& a) {
        std::string path = a.size() > 1 ? a[1] : config::Directory() + "\\state_dump.json";
        if (json::WriteFileAtomic(json::Widen(path), DumpStateJson())) Log::Info("State written to %s", path.c_str());
        else Log::Error("Could not write %s", path.c_str());
    });
}

} // namespace

// =============================================================================
// detail
// =============================================================================
namespace detail {

Fonts& GetFonts() { return g.fonts; }
CoreSettings& Settings() { return g.settings; }
const std::string& SearchQuery() { return g.searchLower; }

float Animate(ImGuiID id, float target, float speed) {
    if (!g.settings.animations) { g.anims[id] = target; return target; }
    auto it = g.anims.find(id);
    if (it == g.anims.end()) { g.anims[id] = target; return target; }
    float dt = ImGui::GetIO().DeltaTime;
    it->second += (target - it->second) * (1.0f - std::exp(-dt * speed));
    if (std::fabs(target - it->second) < 0.0005f) it->second = target;
    return it->second;
}

void ApplyThemeSettings() {
    theme::SetMode(theme::Mode(std::clamp(g.settings.themeMode, 0, 2)));
    theme::SetAccent(g.settings.accent);
}

} // namespace detail

// =============================================================================
// Public API
// =============================================================================
void LoadFonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    g.fonts = detail::Fonts{};

    static const ImWchar iconRanges[] = { 0xE700, 0xF8FF, 0 };
    std::string iconPath = FontPath("SegoeIcons.ttf");
    if (!FileExists(iconPath)) iconPath = FontPath("segmdl2.ttf");
    bool haveIcons = FileExists(iconPath);

    auto add = [&](const char* file, float size, bool icons) -> ImFont* {
        std::string path = FontPath(file);
        if (!FileExists(path)) return nullptr;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        ImFont* f = io.Fonts->AddFontFromFileTTF(path.c_str(), size * scale, &cfg, io.Fonts->GetGlyphRangesDefault());
        if (f && icons && haveIcons) {
            ImFontConfig ic;
            ic.MergeMode = true;
            ic.PixelSnapH = true;
            ic.GlyphOffset = ImVec2(0, size * scale * 0.16f);
            ic.GlyphMinAdvanceX = size * scale;
            io.Fonts->AddFontFromFileTTF(iconPath.c_str(), size * scale * 0.95f, &ic, iconRanges);
        }
        return f;
    };

    g.fonts.regular  = add("segoeui.ttf", 16.0f, true);
    g.fonts.semibold = add("seguisb.ttf", 16.0f, true);
    g.fonts.title    = add("seguisb.ttf", 23.0f, false);
    g.fonts.large    = add("seguisb.ttf", 28.0f, false);
    g.fonts.mono     = add("consola.ttf", 14.0f, false);
    if (!g.fonts.regular) {
        ImFontConfig cfg; cfg.SizePixels = 13.0f * scale;
        g.fonts.regular = io.Fonts->AddFontDefault(&cfg);
    }
    g.fonts.iconsLoaded = haveIcons && g.fonts.regular && FileExists(FontPath("segoeui.ttf"));
    io.FontDefault = g.fonts.regular;
}

bool Initialize(const InitInfo& info) {
    if (g.initialized) return true;
    if (info.manageImGui && (!info.device || !info.context || !info.hwnd)) return false;
    if (!info.device != !info.context) return false;
#ifdef BLAZE_NO_DX11
    if (info.manageImGui || info.device) return false;   // Needs BLAZE_RENDERER_DX11=ON
#endif

    QueryPerformanceFrequency(&g.freq);
    QueryPerformanceCounter(&g.startTime);

    g.info = info;
    g.device = info.device;
    g.context = info.context;
    g.hwnd = info.hwnd;
    g.appName = info.appName ? info.appName : "BlazeImGui";
    g.menuOpen = info.startOpen;
    g.settings.toggleKey = info.toggleKey;
    g.settings.blockInput = info.blockInputWhenOpen;
    g.settings.accent = theme::AccentPresets(nullptr)[0].color;
    if (info.hwnd) {
        HMONITOR mon = MonitorFromWindow(info.hwnd, MONITOR_DEFAULTTONEAREST);
        g.settings.uiScale = std::clamp(ImGui_ImplWin32_GetDpiScaleForMonitor(mon), 1.0f, 2.0f);
    }

    std::string dir = info.configDir ? info.configDir : DefaultConfigDir(g.appName.c_str());
    config::Init(dir);
    debug::Init();
    Log::Info("BlazeImGui %s initializing (config: %s)", kVersion, dir.c_str());

    // Global (menu) settings
    using config::Scope;
    config::Bind("blaze.theme.mode", &g.settings.themeMode, Scope::Global);
    config::Bind("blaze.theme.accent", &g.settings.accent, Scope::Global);
    config::Bind("blaze.ui.scale", &g.settings.uiScale, Scope::Global);
    config::Bind("blaze.ui.animations", &g.settings.animations, Scope::Global);
    config::Bind("blaze.ui.fps_in_header", &g.settings.showFpsInHeader, Scope::Global);
    config::Bind("blaze.ui.last_panel", &g.settings.lastPanel, Scope::Global);
    config::BindKey("blaze.input.toggle_key", &g.settings.toggleKey, Scope::Global);
    config::Bind("blaze.input.block_when_open", &g.settings.blockInput, Scope::Global);
    config::Bind("blaze.input.software_cursor", &g.settings.softwareCursor, Scope::Global);
    config::Bind("blaze.blur.enabled", &g.blur.enabled, Scope::Global);
    config::Bind("blaze.blur.strength", &g.blur.strength, Scope::Global);
    config::Bind("blaze.blur.tint", &g.blur.tintAmount, Scope::Global);
    config::Bind("blaze.blur.noise", &g.blur.noise, Scope::Global);
    config::Bind("blaze.blur.saturation", &g.blur.saturation, Scope::Global);
    config::Bind("blaze.blur.dim", &g.blur.dim, Scope::Global);
    config::Bind("blaze.overlay.enabled", &g.overlay.enabled, Scope::Global);
    config::Bind("blaze.overlay.corner", &g.overlay.corner, Scope::Global);
    config::Bind("blaze.overlay.graph", &g.overlay.showGraph, Scope::Global);
    config::Bind("blaze.overlay.system", &g.overlay.showSystem, Scope::Global);
    config::Bind("blaze.config.autosave", &g.settings.autoSave, Scope::Global);
    config::Bind("blaze.config.last_profile", &g.settings.lastProfile, Scope::Global);
    config::LoadGlobal();
    g.settings.uiScale = std::clamp(g.settings.uiScale, 0.75f, 2.0f);
    config::SetAutoSave(g.settings.autoSave);
    detail::ApplyThemeSettings();

    if (info.manageImGui) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        g.iniPath = dir + "\\imgui_layout.ini";
        io.IniFilename = g.iniPath.c_str();
        ImGui_ImplWin32_Init(info.hwnd);
#ifndef BLAZE_NO_DX11
        ImGui_ImplDX11_Init(info.device, info.context);
#endif
        LoadFonts(g.settings.uiScale);
    }
    theme::ApplyStyleMetrics(g.settings.uiScale);
    g.appliedScale = g.settings.uiScale;
    theme::Update(1.0f);

    // GPU name / VRAM via the given adapter, or the device's adapter
    IDXGIAdapter* adapter = info.adapter;
    if (adapter) adapter->AddRef();
#ifndef BLAZE_NO_DX11
    IDXGIDevice* dxgiDevice = nullptr;
    if (!adapter && info.device &&
        SUCCEEDED(info.device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice))) {
        dxgiDevice->GetAdapter(&adapter);
        dxgiDevice->Release();
    }
#endif
    perf::Init(adapter);
    if (adapter) adapter->Release();

    if (!info.device)
        Log::Info("No D3D11 device - menu runs without acrylic blur");
    else if (!detail::blur::Init(info.device, info.context))
        Log::Warn("Acrylic blur unavailable (shader compile failed) - continuing without it");

    if (info.registerBuiltinPanels) {
        detail::RegisterPerformancePanel();
        detail::RegisterDebugPanel();
        detail::RegisterConfigPanel();
        detail::RegisterSettingsPanel();
    }
    RegisterCoreCommands();

    // Profiles: load last used, or create "default".
    if (!config::LoadProfile(g.settings.lastProfile)) {
        auto profiles = config::ListProfiles();
        if (!profiles.empty()) config::LoadProfile(profiles.front());
        else config::SaveProfile("default");
    }
    config::OnProfileLoaded([](const std::string& name) { g.settings.lastProfile = name; });
    g.settings.lastProfile = config::ActiveProfile();
    if (!g.settings.lastPanel.empty()) g.selected = g.settings.lastPanel;

    g.visibility = g.menuOpen ? 1.0f : 0.0f;
    g.initialized = true;
    Log::Info("BlazeImGui ready - press %s to toggle the menu", ui::VirtualKeyName(g.settings.toggleKey));
    return true;
}

void Shutdown() {
    if (!g.initialized) return;
    config::Shutdown();
    detail::blur::Shutdown();
    perf::Shutdown();
    debug::Shutdown();
    if (g.info.manageImGui) {
#ifndef BLAZE_NO_DX11
        ImGui_ImplDX11_Shutdown();
#endif
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    g = State{};
}

bool IsInitialized() { return g.initialized; }

void NewFrame() {
    if (!g.initialized) return;
    if (g.info.manageImGui) {
        if (g.fontsDirty) {
            g.fontsDirty = false;
            LoadFonts(g.settings.uiScale);
#ifndef BLAZE_NO_DX11
            ImGui_ImplDX11_InvalidateDeviceObjects();   // NewFrame recreates the font texture
#endif
        }
#ifndef BLAZE_NO_DX11
        ImGui_ImplDX11_NewFrame();
#endif
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }
    DrawUI();
}

void DrawUI() {
    if (!g.initialized) return;
    ImGuiIO& io = ImGui::GetIO();
    float dt = std::max(io.DeltaTime, 0.0001f);

    // Theme fields can change from the UI, console (blaze.set) or a config load.
    const ImVec4& ac = g.settings.accent;
    if (g.appliedMode != g.settings.themeMode || ac.x != g.appliedAccent.x || ac.y != g.appliedAccent.y || ac.z != g.appliedAccent.z) {
        g.appliedMode = g.settings.themeMode;
        g.appliedAccent = ac;
        detail::ApplyThemeSettings();
    }
    if (g.appliedScale != g.settings.uiScale) {
        // Style metrics apply immediately; fonts are rebuilt at the start of next frame.
        g.appliedScale = g.settings.uiScale;
        theme::ApplyStyleMetrics(g.settings.uiScale);
        g.fontsDirty = true;   // NewFrame (manageImGui) or the host via ConsumeFontRebuild()
    }

    perf::BeginFrame();
    theme::Update(dt);
    config::SetAutoSave(g.settings.autoSave);
    config::Tick(dt);
    PollToggleKey();

    float target = g.menuOpen ? 1.0f : 0.0f;
    g.visibility = g.settings.animations ? g.visibility + (target - g.visibility) * (1.0f - std::exp(-dt * 14.0f)) : target;
    if (std::fabs(g.visibility - target) < 0.002f) g.visibility = target;
    io.MouseDrawCursor = g.menuOpen && g.settings.softwareCursor;

    if (g.visibility > 0.001f) DrawMainWindow();
    if (g.info.registerBuiltinPanels) detail::DrawDebugToolWindows();
    DrawOverlay();
    DrawToasts();
}

void RenderBlur(ID3D11RenderTargetView* target) {
    if (!g.initialized || !target) return;
    ImVec4 tint = C().background;
    detail::blur::Apply(target, g.blur, tint, g.visibility);
}

void Render(ID3D11RenderTargetView* target) {
    if (!g.initialized) return;
    RenderBlur(target);
    ImGui::Render();
    if (!g.info.manageImGui) return;
#ifndef BLAZE_NO_DX11
    // Bind the target for ImGui (the backend does not), then restore the caller's targets.
    ID3D11RenderTargetView* prevRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* prevDsv = nullptr;
    g.context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRtv, &prevDsv);
    if (target) g.context->OMSetRenderTargets(1, &target, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g.context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, prevRtv, prevDsv);
    for (auto* r : prevRtv) if (r) r->Release();
    if (prevDsv) prevDsv->Release();
#endif
}

bool WndProcHandler(HWND hwnd, unsigned int msg, uintptr_t wParam, intptr_t lParam) {
    if (!g.initialized) return false;
    if (g.info.manageImGui) ImGui_ImplWin32_WndProcHandler(hwnd, msg, WPARAM(wParam), LPARAM(lParam));

    // Swallow the toggle key itself so the game never sees it.
    bool isKeyMsg = msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
    if (isKeyMsg && int(wParam) == g.settings.toggleKey) return true;

    if (!g.menuOpen || !g.settings.blockInput) return false;
    if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST))
        return true;
    return false;
}

void OnResizeBegin() { detail::blur::ReleaseTargets(); }
void OnResizeEnd() {}

bool ConsumeFontRebuild() {
    if (g.info.manageImGui || !g.fontsDirty) return false;
    g.fontsDirty = false;
    return true;
}

int GetToggleKey() { return g.settings.toggleKey; }

void SetMenuOpen(bool open) { g.menuOpen = open; }
bool IsMenuOpen() { return g.menuOpen; }
void ToggleMenu() { g.menuOpen = !g.menuOpen; }
float MenuVisibility() { return g.visibility; }

void RegisterPanel(const Panel& panel) {
    if (panel.id.empty()) return;
    UnregisterPanel(panel.id);
    g.panels.push_back(panel);
    SortPanels();
}

void UnregisterPanel(const std::string& id) {
    g.panels.erase(std::remove_if(g.panels.begin(), g.panels.end(), [&](auto& p) { return p.id == id; }), g.panels.end());
}

void SelectPanel(const std::string& id) {
    if (g.selected == id) return;
    g.selected = id;
    g.settings.lastPanel = id;
    g.pageAnim = 0;
}

const std::string& SelectedPanel() { return g.selected; }

void SetCategoryOrder(const std::vector<std::string>& categories) {
    g.categoryOrder = categories;
    SortPanels();
}

void Notify(const char* message, ToastKind kind, float seconds) {
    g.toasts.push_back({ message ? message : "", kind, seconds, seconds });
    if (g.toasts.size() > 6) g.toasts.erase(g.toasts.begin());
}

BlurSettings& Blur() { return g.blur; }
bool BlurAvailable() { return detail::blur::Available(); }
OverlaySettings& Overlay() { return g.overlay; }

void SetUIScale(float scale) { g.settings.uiScale = std::clamp(scale, 0.75f, 2.0f); }
float GetUIScale() { return g.settings.uiScale > 0 ? g.settings.uiScale : 1.0f; }

double Time() {
    if (!g.freq.QuadPart) return 0;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    return double(now.QuadPart - g.startTime.QuadPart) / double(g.freq.QuadPart);
}

std::string DumpStateJson() {
    json::Value root = json::Value::MakeObject();
    root["blaze_version"] = json::Value(kVersion);
    root["app"] = json::Value(g.appName);
    root["time_seconds"] = json::Value(Time());
    root["menu_open"] = json::Value(g.menuOpen);
    json::Value& win = root["main_window"];
    win["x"] = json::Value(double(g.mainPos.x)); win["y"] = json::Value(double(g.mainPos.y));
    win["w"] = json::Value(double(g.mainSize.x)); win["h"] = json::Value(double(g.mainSize.y));
    root["selected_panel"] = json::Value(g.selected);
    root["profile"] = json::Value(config::ActiveProfile());

    const auto& fs = perf::Frame();
    json::Value& perfJ = root["performance"];
    perfJ["fps"] = json::Value(double(fs.fps));
    perfJ["frame_ms"] = json::Value(double(fs.frameMs));
    perfJ["avg_ms"] = json::Value(double(fs.avgMs));
    perfJ["min_ms"] = json::Value(double(fs.minMs));
    perfJ["max_ms"] = json::Value(double(fs.maxMs));
    perfJ["low_1pct_fps"] = json::Value(double(fs.low1PctFps));
    const auto& ss = perf::System();
    perfJ["cpu_process_pct"] = json::Value(double(ss.cpuProcessPct));
    perfJ["working_set_mb"] = json::Value(ss.workingSetMB);
    perfJ["vram_used_mb"] = json::Value(ss.vramUsedMB);
    perfJ["gpu"] = json::Value(ss.gpuName);
    json::Value scopes = json::Value::MakeArray();
    for (auto& s : perf::Scopes()) {
        json::Value o = json::Value::MakeObject();
        o["name"] = json::Value(s.name); o["avg_ms"] = json::Value(double(s.avgMs));
        o["max_ms"] = json::Value(double(s.maxMs)); o["calls"] = json::Value(s.calls);
        scopes.push(o);
    }
    perfJ["scopes"] = scopes;
    json::Value counters = json::Value::MakeObject();
    for (auto& c : perf::Counters()) counters[c.name] = json::Value(c.value);
    perfJ["counters"] = counters;

    json::Value vars = json::Value::MakeObject();
    for (auto& v : config::ListVars()) {
        json::Value parsed;
        json::Parse(v.valueJson, parsed);
        vars[v.key] = parsed;
    }
    root["config_vars"] = vars;

    json::Value watches = json::Value::MakeObject();
    for (auto& w : debug::Watches()) watches[w.label] = json::Value(w.getter ? w.getter() : std::string());
    root["watches"] = watches;

    json::Value logs = json::Value::MakeArray();
    static const char* levels[] = { "trace", "info", "warn", "error" };
    for (auto& e : Log::Snapshot(100)) {
        json::Value o = json::Value::MakeObject();
        o["t"] = json::Value(e.time); o["level"] = json::Value(levels[int(e.level)]);
        o["cat"] = json::Value(e.category); o["msg"] = json::Value(e.message);
        logs.push(o);
    }
    root["recent_log"] = logs;
    return root.dump(2);
}

} // namespace blaze
