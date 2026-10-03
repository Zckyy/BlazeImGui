#include "internal.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace blaze::ui {
namespace {

using theme::U32;
using theme::WithAlpha;

inline float S(float v) { return v * GetUIScale(); }
inline const theme::Palette& C() { return theme::Colors(); }

int  g_cardRows = 0;          // Rows drawn in the current card (for separators)
bool g_inCard = false;
bool g_columnsOpen = false;

// Settings-row layout ------------------------------------------------------------
struct Row {
    ImVec2 pos;
    float  width;
    float  height;
    bool   hovered;
};

constexpr float kControlW = 220.0f;  // Right-hand control column width (unscaled)

Row BeginRow(const char* label, const char* desc, float controlH, float controlW) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = win->DrawList;
    Row r;
    r.pos = ImGui::GetCursorScreenPos();
    r.width = ImGui::GetContentRegionAvail().x;

    const float lineH = ImGui::GetTextLineHeight();
    const float textH = lineH + (desc && *desc ? lineH + S(1) : 0);
    const float padY = S(desc && *desc ? 7.0f : 6.0f);
    r.height = std::max(textH, controlH) + padY * 2;
    r.hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                ImGui::IsMouseHoveringRect(r.pos, ImVec2(r.pos.x + r.width, r.pos.y + r.height));

    if (g_inCard && g_cardRows > 0)
        dl->AddLine(ImVec2(r.pos.x, r.pos.y), ImVec2(r.pos.x + r.width, r.pos.y), U32(C().border));
    if (g_inCard) ++g_cardRows;

    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    float textY = r.pos.y + (r.height - textH) * 0.5f;
    ImVec4 clip(r.pos.x, r.pos.y, r.pos.x + r.width - controlW - S(12), r.pos.y + r.height);
    dl->AddText(nullptr, 0, ImVec2(r.pos.x, textY), U32(C().text), label, labelEnd, 0, &clip);
    if (desc && *desc)
        dl->AddText(nullptr, 0, ImVec2(r.pos.x, textY + lineH + S(1)), U32(C().textMuted), desc, nullptr, 0, &clip);
    return r;
}

// Positions the cursor for the right-aligned control.
void PlaceControl(const Row& r, float controlW, float controlH) {
    ImGui::SetCursorScreenPos(ImVec2(r.pos.x + r.width - controlW, r.pos.y + (r.height - controlH) * 0.5f));
}

void EndRow(const Row& r) {
    ImGui::SetCursorScreenPos(r.pos);
    ImGui::Dummy(ImVec2(r.width, r.height));
}

void DrawFrameRect(ImDrawList* dl, ImVec2 a, ImVec2 b, bool hovered, bool active, float rounding) {
    ImVec4 bg = active ? C().surfaceActive : hovered ? C().controlHover : C().control;
    dl->AddRectFilled(a, b, U32(bg), rounding);
    dl->AddRect(a, b, U32(C().border), rounding);
}

// std::string resize callback for InputText
int StringResizeCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* str = static_cast<std::string*>(data->UserData);
        str->resize(size_t(data->BufTextLen));
        data->Buf = str->data();
    }
    return 0;
}

// Slider core shared by float/int variants. Returns true on change.
bool SliderCore(const char* id, float* v, float vmin, float vmax, float width, float height) {
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImDrawList* dl = win->DrawList;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(width, height));
    bool hovered = ImGui::IsItemHovered();
    bool active = ImGui::IsItemActive();
    ImGuiID gid = ImGui::GetItemID();

    bool changed = false;
    const float knobR = S(8);
    const float x0 = pos.x + knobR, x1 = pos.x + width - knobR;
    if (active) {
        float t = std::clamp((ImGui::GetIO().MousePos.x - x0) / std::max(1.0f, x1 - x0), 0.0f, 1.0f);
        float nv = vmin + (vmax - vmin) * t;
        if (nv != *v) { *v = nv; changed = true; }
    }
    if (hovered && !active && ImGui::GetIO().MouseWheel != 0 && ImGui::GetIO().KeyCtrl) {
        *v = std::clamp(*v + (vmax - vmin) * 0.02f * ImGui::GetIO().MouseWheel, vmin, vmax);
        changed = true;
    }

    float t = vmax > vmin ? std::clamp((*v - vmin) / (vmax - vmin), 0.0f, 1.0f) : 0.0f;
    float tAnim = detail::Animate(gid, t, active ? 40.0f : 18.0f);
    float cy = pos.y + height * 0.5f;
    float trackH = S(4);
    float kx = x0 + (x1 - x0) * tAnim;

    dl->AddRectFilled(ImVec2(x0, cy - trackH / 2), ImVec2(x1, cy + trackH / 2), U32(C().control), trackH);
    dl->AddRectFilled(ImVec2(x0, cy - trackH / 2), ImVec2(kx, cy + trackH / 2), U32(C().accent), trackH);

    // Windows 11 style knob: outer ring + accent core that grows on hover.
    float hoverT = detail::Animate(gid + 1, (hovered || active) ? 1.0f : 0.0f);
    ImVec4 ring = theme::IsDark() ? ImVec4(0.27f, 0.27f, 0.30f, 1) : ImVec4(1, 1, 1, 1);
    dl->AddCircleFilled(ImVec2(kx, cy), knobR + S(1), U32(C().shadow, 0.5f), 24);
    dl->AddCircleFilled(ImVec2(kx, cy), knobR, U32(ring), 24);
    float core = active ? S(3.5f) : S(4.0f) + S(1.5f) * hoverT;
    dl->AddCircleFilled(ImVec2(kx, cy), core, U32(active ? C().accentActive : C().accent), 20);
    return changed;
}

} // namespace

// =============================================================================
// Fonts
// =============================================================================
ImFont* GetFont(Font f) {
    auto& fonts = detail::GetFonts();
    ImFont* r = nullptr;
    switch (f) {
    case Font::Regular:  r = fonts.regular; break;
    case Font::Semibold: r = fonts.semibold; break;
    case Font::Title:    r = fonts.title; break;
    case Font::Large:    r = fonts.large; break;
    case Font::Mono:     r = fonts.mono; break;
    }
    return r ? r : ImGui::GetFont();
}
void PushFont(Font f) { ImGui::PushFont(GetFont(f)); }
void PopFont() { ImGui::PopFont(); }

// =============================================================================
// Containers
// =============================================================================
bool BeginCard(const char* title, const char* icon, const char* subtitle) {
    IM_ASSERT(!g_inCard && "blaze::ui::BeginCard cannot be nested");
    ImGui::PushStyleColor(ImGuiCol_ChildBg, C().surface);
    ImGui::PushStyleColor(ImGuiCol_Border, C().border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(12)));
    bool open = ImGui::BeginChild(title ? title : "##card", ImVec2(0, 0),
        ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    g_inCard = true;
    g_cardRows = 0;
    if (!open) return false;

    if (title && *title && ImGui::FindRenderedTextEnd(title) != title) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float x = p.x;
        PushFont(Font::Semibold);
        float lh = ImGui::GetTextLineHeight();
        if (icon && *icon && detail::GetFonts().iconsLoaded) {
            dl->AddText(ImVec2(x, p.y), U32(C().accent), icon);
            x += ImGui::CalcTextSize(icon).x + S(10);
        }
        dl->AddText(ImVec2(x, p.y), U32(C().text), title, ImGui::FindRenderedTextEnd(title));
        PopFont();
        float h = lh;
        if (subtitle && *subtitle) {
            dl->AddText(ImVec2(x, p.y + lh + S(1)), U32(C().textMuted), subtitle);
            h += ImGui::GetTextLineHeight() + S(1);
        }
        ImGui::Dummy(ImVec2(0, h + S(2)));
    }
    return true;
}

void EndCard() {
    IM_ASSERT(g_inCard && "blaze::ui::EndCard without BeginCard");
    g_inCard = false;
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, S(2)));
}

void SectionHeader(const char* text) {
    ImGui::Dummy(ImVec2(0, S(4)));
    PushFont(Font::Semibold);
    ImGui::PushStyleColor(ImGuiCol_Text, C().text);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
    PopFont();
}

void PageHeader(const char* title, const char* subtitle) {
    PushFont(Font::Title);
    ImGui::PushStyleColor(ImGuiCol_Text, C().text);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    PopFont();
    if (subtitle && *subtitle) TextMuted("%s", subtitle);
    ImGui::Dummy(ImVec2(0, S(6)));
}

void BeginColumns(int count, const char* id) {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(5), S(5)));
    g_columnsOpen = ImGui::BeginTable(id, std::max(1, count), ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoPadOuterX);
    if (g_columnsOpen) ImGui::TableNextColumn();
}
void NextColumn() { if (g_columnsOpen) ImGui::TableNextColumn(); }
void EndColumns() {
    if (g_columnsOpen) ImGui::EndTable();
    g_columnsOpen = false;
    ImGui::PopStyleVar();
}

// =============================================================================
// Setting rows
// =============================================================================
bool Toggle(const char* label, bool* v, const char* description) {
    ImGui::PushID(label);
    const float w = S(40), h = S(20);
    Row r = BeginRow(label, description, h, w);

    // The whole row is clickable, like Windows Settings.
    ImGui::SetCursorScreenPos(r.pos);
    bool pressed = ImGui::InvisibleButton("##row", ImVec2(r.width, r.height));
    if (pressed) *v = !*v;
    bool hovered = ImGui::IsItemHovered();
    ImGuiID id = ImGui::GetItemID();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float t = detail::Animate(id, *v ? 1.0f : 0.0f);
    ImVec2 a(r.pos.x + r.width - w, r.pos.y + (r.height - h) * 0.5f);
    ImVec2 b(a.x + w, a.y + h);
    ImVec4 offBg = hovered ? C().controlHover : C().control;
    ImVec4 onBg = hovered ? C().accentHover : C().accent;
    dl->AddRectFilled(a, b, U32(theme::Lerp(offBg, onBg, t)), h);
    if (t < 0.99f) dl->AddRect(a, b, U32(C().textMuted, (1 - t) * 0.6f), h, 0, S(1));

    float knobR = h * 0.5f - S(hovered ? 3.0f : 4.0f);
    float kx = a.x + h * 0.5f + (w - h) * t;
    ImVec4 knob = theme::Lerp(C().textMuted, C().accentText, t);
    dl->AddCircleFilled(ImVec2(kx, a.y + h * 0.5f), knobR, U32(knob), 20);

    EndRow(r);
    ImGui::PopID();
    return pressed;
}

bool SliderFloat(const char* label, float* v, float vmin, float vmax, const char* fmt, const char* description) {
    ImGui::PushID(label);
    const float cw = S(kControlW), ch = S(20);
    Row r = BeginRow(label, description, ch, cw);

    char buf[64];
    snprintf(buf, sizeof buf, fmt, *v);
    float valueW = S(52);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 ts = ImGui::CalcTextSize(buf);
    float baseX = r.pos.x + r.width - cw;
    dl->AddText(ImVec2(baseX + valueW - ts.x - S(8), r.pos.y + (r.height - ts.y) * 0.5f), U32(C().textMuted), buf);

    ImGui::SetCursorScreenPos(ImVec2(baseX + valueW, r.pos.y + (r.height - ch) * 0.5f));
    bool changed = SliderCore("##slider", v, vmin, vmax, cw - valueW, ch);

    // Double-click to type an exact value.
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) ImGui::OpenPopup("##edit");
    if (ImGui::BeginPopup("##edit")) {
        ImGui::SetNextItemWidth(S(140));
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputFloat("##val", v, 0, 0, fmt, ImGuiInputTextFlags_EnterReturnsTrue)) {
            changed = true; ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::IsItemHovered() && !ImGui::IsItemActive()) ImGui::SetTooltip("Double-click to type a value");

    EndRow(r);
    ImGui::PopID();
    return changed;
}

bool SliderInt(const char* label, int* v, int vmin, int vmax, const char* fmt, const char* description) {
    ImGui::PushID(label);
    const float cw = S(kControlW), ch = S(20);
    Row r = BeginRow(label, description, ch, cw);

    char buf[64];
    snprintf(buf, sizeof buf, fmt, *v);
    float valueW = S(52);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 ts = ImGui::CalcTextSize(buf);
    float baseX = r.pos.x + r.width - cw;
    dl->AddText(ImVec2(baseX + valueW - ts.x - S(8), r.pos.y + (r.height - ts.y) * 0.5f), U32(C().textMuted), buf);

    ImGui::SetCursorScreenPos(ImVec2(baseX + valueW, r.pos.y + (r.height - ch) * 0.5f));
    float f = float(*v);
    bool changed = false;
    if (SliderCore("##slider", &f, float(vmin), float(vmax), cw - valueW, ch)) {
        int nv = int(std::lround(f));
        if (nv != *v) { *v = nv; changed = true; }
    }
    EndRow(r);
    ImGui::PopID();
    return changed;
}

bool Combo(const char* label, int* current, const char* const items[], int count, const char* description) {
    ImGui::PushID(label);
    const float cw = S(kControlW) * 0.8f, ch = ImGui::GetFrameHeight();
    Row r = BeginRow(label, description, ch, cw);
    PlaceControl(r, cw, ch);

    bool changed = false;
    ImGui::SetNextItemWidth(cw);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, C().border);
    const char* preview = (*current >= 0 && *current < count) ? items[*current] : "";
    ImVec2 cp = ImGui::GetCursorScreenPos();
    bool open = ImGui::BeginCombo("##combo", preview, ImGuiComboFlags_NoArrowButton);
    {
        // Fluent-style chevron instead of ImGui's arrow button.
        const char* chev = detail::GetFonts().iconsLoaded ? icons::ChevronDown.c_str() : "v";
        ImFont* f = ImGui::GetFont();
        float fs = ImGui::GetFontSize() * 0.75f;
        ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, chev);
        ImGui::GetWindowDrawList()->AddText(f, fs, ImVec2(cp.x + cw - ts.x - S(10), cp.y + (ch - ts.y) * 0.5f + S(1)),
                                            U32(C().textMuted), chev);
    }
    if (open) {
        for (int i = 0; i < count; ++i) {
            bool sel = i == *current;
            if (ImGui::Selectable(items[i], sel)) { *current = i; changed = true; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    EndRow(r);
    ImGui::PopID();
    return changed;
}

bool ColorEdit(const char* label, ImVec4* color, const char* description) {
    ImGui::PushID(label);
    const float cw = S(44), ch = S(24);
    Row r = BeginRow(label, description, ch, cw);
    PlaceControl(r, cw, ch);

    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##swatch", ImVec2(cw, ch));
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Checkerboard hint for alpha
    if (color->w < 1.0f) {
        dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch), IM_COL32(200, 200, 200, 255), S(6));
        for (int i = 0; i < 6; ++i)
            dl->AddRectFilled(ImVec2(p.x + i * cw / 6, p.y + ((i & 1) ? ch / 2 : 0)),
                              ImVec2(p.x + (i + 1) * cw / 6, p.y + ((i & 1) ? ch : ch / 2)), IM_COL32(150, 150, 150, 255));
    }
    dl->AddRectFilled(p, ImVec2(p.x + cw, p.y + ch), ImGui::ColorConvertFloat4ToU32(*color), S(6));
    dl->AddRect(p, ImVec2(p.x + cw, p.y + ch), U32(hovered ? C().textMuted : C().border), S(6), 0, S(1));
    if (clicked) ImGui::OpenPopup("##picker");

    bool changed = false;
    if (ImGui::BeginPopup("##picker")) {
        changed = ImGui::ColorPicker4("##pick", &color->x,
            ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_PickerHueWheel |
            ImGuiColorEditFlags_DisplayHex);
        ImGui::EndPopup();
    }
    EndRow(r);
    ImGui::PopID();
    return changed;
}

const char* VirtualKeyName(int vk) {
    static char buf[64];
    switch (vk) {
    case 0:          return "None";
    case VK_LBUTTON: return "Mouse 1";
    case VK_RBUTTON: return "Mouse 2";
    case VK_MBUTTON: return "Mouse 3";
    case VK_XBUTTON1: return "Mouse 4";
    case VK_XBUTTON2: return "Mouse 5";
    case VK_INSERT:  return "Insert";
    case VK_DELETE:  return "Delete";
    case VK_HOME:    return "Home";
    case VK_END:     return "End";
    case VK_PRIOR:   return "Page Up";
    case VK_NEXT:    return "Page Down";
    case VK_LEFT:    return "Left";
    case VK_RIGHT:   return "Right";
    case VK_UP:      return "Up";
    case VK_DOWN:    return "Down";
    case VK_OEM_3:   return "` (Tilde)";
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:       return "Shift";
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return "Ctrl";
    case VK_MENU: case VK_LMENU: case VK_RMENU:          return "Alt";
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(buf, sizeof buf, "F%d", vk - VK_F1 + 1); return buf; }
    UINT scan = MapVirtualKeyW(UINT(vk), MAPVK_VK_TO_VSC);
    wchar_t wname[64];
    if (scan && GetKeyNameTextW(LONG(scan << 16), wname, 64) > 0) {
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, buf, sizeof buf, nullptr, nullptr);
        return buf;
    }
    snprintf(buf, sizeof buf, "Key 0x%02X", vk);
    return buf;
}

bool KeyBind(const char* label, int* vk, const char* description) {
    static ImGuiID s_listening = 0;
    static bool    s_waitRelease = false;

    ImGui::PushID(label);
    const float cw = S(130), ch = ImGui::GetFrameHeight();
    Row r = BeginRow(label, description, ch, cw);
    PlaceControl(r, cw, ch);

    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton("##bind", ImVec2(cw, ch));
    ImGuiID id = ImGui::GetItemID();
    bool hovered = ImGui::IsItemHovered();
    bool listening = s_listening == id;
    if (clicked && !listening) { s_listening = id; s_waitRelease = true; listening = true; }

    bool changed = false;
    if (listening) {
        // Poll the hardware state so this works even when input is routed elsewhere (hooks).
        bool anyDown = false;
        for (int k = 1; k < 255; ++k) {
            if (!(GetAsyncKeyState(k) & 0x8000)) continue;
            anyDown = true;
            if (s_waitRelease) break;
            if (k == VK_LBUTTON || k == VK_RBUTTON) continue;        // reserved for UI interaction
            if (k == VK_ESCAPE) { s_listening = 0; break; }
            if (k == VK_BACK) { *vk = 0; changed = true; s_listening = 0; break; }
            if (k == VK_SHIFT || k == VK_CONTROL || k == VK_MENU) continue; // prefer L/R variants
            *vk = k; changed = true; s_listening = 0; break;
        }
        if (!anyDown) s_waitRelease = false;
        if (ImGui::IsMouseClicked(0) && !hovered) s_listening = 0;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(p.x + cw, p.y + ch);
    if (listening) {
        float pulse = 0.5f + 0.5f * std::sin(float(ImGui::GetTime()) * 6.0f);
        dl->AddRectFilled(p, b, U32(C().accent, 0.15f + 0.1f * pulse), S(6));
        dl->AddRect(p, b, U32(C().accent), S(6), 0, S(1.5f));
    } else {
        DrawFrameRect(dl, p, b, hovered, false, S(6));
    }
    const char* text = listening ? "Press a key..." : VirtualKeyName(*vk);
    ImVec2 ts = ImGui::CalcTextSize(text);
    dl->AddText(ImVec2(p.x + (cw - ts.x) * 0.5f, p.y + (ch - ts.y) * 0.5f),
                U32(listening ? C().accent : C().text), text);
    if (hovered && !listening) ImGui::SetTooltip("Click, then press a key. Esc cancels, Backspace clears.");

    EndRow(r);
    ImGui::PopID();
    return changed;
}

bool InputText(const char* label, std::string* str, const char* hint, const char* description) {
    ImGui::PushID(label);
    const float cw = S(kControlW), ch = ImGui::GetFrameHeight();
    Row r = BeginRow(label, description, ch, cw);
    PlaceControl(r, cw, ch);
    ImGui::SetNextItemWidth(cw);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, C().border);
    bool changed = ImGui::InputTextWithHint("##input", hint ? hint : "", str->data(), str->capacity() + 1,
        ImGuiInputTextFlags_CallbackResize, StringResizeCallback, str);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    EndRow(r);
    ImGui::PopID();
    return changed;
}

// =============================================================================
// Buttons
// =============================================================================
bool Button(const char* label, ButtonKind kind, const ImVec2& sizeIn) {
    const char* end = ImGui::FindRenderedTextEnd(label);
    ImVec2 ts = ImGui::CalcTextSize(label, end);
    ImVec2 size(sizeIn.x > 0 ? sizeIn.x : ts.x + S(28), sizeIn.y > 0 ? sizeIn.y : ImGui::GetFrameHeight() + S(2));
    if (sizeIn.x < 0) size.x = ImGui::GetContentRegionAvail().x;

    ImVec2 p = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::InvisibleButton(label, size);
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();
    float hv = detail::Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 20.0f);

    ImVec4 bg, fg, border(0, 0, 0, 0);
    switch (kind) {
    case ButtonKind::Primary:
        bg = held ? C().accentActive : theme::Lerp(C().accent, C().accentHover, hv);
        fg = C().accentText;
        break;
    case ButtonKind::Secondary:
        bg = held ? C().surfaceActive : theme::Lerp(C().control, C().controlHover, hv);
        fg = C().text; border = C().border;
        break;
    case ButtonKind::Subtle:
        bg = held ? C().surfaceActive : WithAlpha(C().controlHover, C().controlHover.w * hv);
        fg = C().text;
        break;
    case ButtonKind::Danger:
        bg = held ? WithAlpha(C().error, 0.85f) : theme::Lerp(WithAlpha(C().error, 0.16f), C().error, hv);
        fg = hv > 0.5f ? ImVec4(1, 1, 1, 1) : C().error;
        break;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(p.x + size.x, p.y + size.y);
    dl->AddRectFilled(p, b, U32(bg), S(6));
    if (border.w > 0) dl->AddRect(p, b, U32(border), S(6));
    dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), U32(fg), label, end);
    return pressed;
}

bool IconButton(const char* icon, const char* tooltip, bool active, float size) {
    float sz = size > 0 ? size : ImGui::GetFrameHeight() + S(4);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(icon);
    ImGui::PushID(tooltip ? tooltip : "");
    bool pressed = ImGui::InvisibleButton("##icon", ImVec2(sz, sz));
    bool hovered = ImGui::IsItemHovered();
    bool held = ImGui::IsItemActive();
    float hv = detail::Animate(ImGui::GetItemID(), hovered ? 1.0f : 0.0f, 20.0f);
    ImGui::PopID();
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(p.x + sz, p.y + sz);
    if (active) dl->AddRectFilled(p, b, U32(C().accent, 0.18f), S(6));
    if (hv > 0.01f || held) dl->AddRectFilled(p, b, U32(held ? C().surfaceActive : C().controlHover, held ? 1.0f : hv), S(6));
    const char* glyph = detail::GetFonts().iconsLoaded ? icon : "?";
    ImVec2 ts = ImGui::CalcTextSize(glyph);
    dl->AddText(ImVec2(p.x + (sz - ts.x) * 0.5f, p.y + (sz - ts.y) * 0.5f),
                U32(active ? C().accent : theme::Lerp(C().textMuted, C().text, hv)), glyph);
    if (tooltip && hovered) ImGui::SetTooltip("%s", tooltip);
    return pressed;
}

bool Segmented(const char* id, int* current, const char* const items[], int count, float width) {
    if (count <= 0) return false;
    ImGui::PushID(id);
    float h = ImGui::GetFrameHeight() + S(2);
    float w = width > 0 ? width : 0;
    if (w <= 0) for (int i = 0; i < count; ++i) w += ImGui::CalcTextSize(items[i]).x + S(28);
    float segW = w / count;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), U32(C().control), S(7));
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), U32(C().border), S(7));

    bool changed = false;
    ImGuiID animId = ImGui::GetID("##pill");
    float pillX = detail::Animate(animId, float(*current) * segW, 18.0f);
    ImVec2 pa(p.x + pillX + S(3), p.y + S(3)), pb(p.x + pillX + segW - S(3), p.y + h - S(3));
    dl->AddRectFilled(ImVec2(pa.x, pa.y + S(1)), ImVec2(pb.x, pb.y + S(1)), U32(C().shadow, 0.35f), S(5));
    dl->AddRectFilled(pa, pb, U32(theme::IsDark() ? ImVec4(1, 1, 1, 0.12f) : ImVec4(1, 1, 1, 1)), S(5));

    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + segW * i, p.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##seg", ImVec2(segW, h)) && *current != i) { *current = i; changed = true; }
        bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        ImVec2 ts = ImGui::CalcTextSize(items[i]);
        ImVec4 col = i == *current ? C().text : (hovered ? C().text : C().textMuted);
        dl->AddText(ImVec2(p.x + segW * i + (segW - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), U32(col), items[i]);
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
    return changed;
}

// =============================================================================
// Display
// =============================================================================
static void GradientArea(ImDrawList* dl, const ImVec2* pts, int n, float bottom, ImU32 top, ImU32 bot) {
    if (n < 2) return;
    ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve((n - 1) * 6, (n - 1) * 4);
    for (int i = 0; i < n - 1; ++i) {
        ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
        dl->PrimWriteVtx(pts[i], uv, top);
        dl->PrimWriteVtx(pts[i + 1], uv, top);
        dl->PrimWriteVtx(ImVec2(pts[i + 1].x, bottom), uv, bot);
        dl->PrimWriteVtx(ImVec2(pts[i].x, bottom), uv, bot);
        dl->PrimWriteIdx(base); dl->PrimWriteIdx(ImDrawIdx(base + 1)); dl->PrimWriteIdx(ImDrawIdx(base + 2));
        dl->PrimWriteIdx(base); dl->PrimWriteIdx(ImDrawIdx(base + 2)); dl->PrimWriteIdx(ImDrawIdx(base + 3));
    }
}

static void DrawSeries(ImDrawList* dl, const float* values, int count, float vmin, float vmax,
                       ImVec2 a, ImVec2 b, const ImVec4& color, float thickness, ImVec2* outPts) {
    float range = std::max(1e-6f, vmax - vmin);
    for (int i = 0; i < count; ++i) {
        float t = count > 1 ? float(i) / float(count - 1) : 0.0f;
        float v = std::clamp((values[i] - vmin) / range, 0.0f, 1.0f);
        outPts[i] = ImVec2(a.x + (b.x - a.x) * t, b.y - (b.y - a.y) * v);
    }
    GradientArea(dl, outPts, count, b.y, U32(color, 0.28f), U32(color, 0.0f));
    dl->AddPolyline(outPts, count, U32(color), ImDrawFlags_None, thickness);
}

void Sparkline(const char* id, const float* values, int count, float vmin, float vmax, const ImVec2& size, const ImVec4& color) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    (void)id;
    if (count < 2 || !ImGui::IsItemVisible()) return;
    ImVector<ImVec2> pts; pts.resize(count);
    DrawSeries(ImGui::GetWindowDrawList(), values, count, vmin, vmax, p, ImVec2(p.x + size.x, p.y + size.y), color, S(1.5f), pts.Data);
}

void LineGraph(const char* id, const float* values, int count, float vmin, float vmax, const ImVec2& sizeIn,
               const ImVec4& color, const GraphGuide* guides, int guideCount, const char* unit) {
    ImVec2 size(sizeIn.x > 0 ? sizeIn.x : ImGui::GetContentRegionAvail().x, sizeIn.y);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    if (!ImGui::IsItemVisible()) return;
    bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(p.x + size.x, p.y + size.y);

    dl->AddRectFilled(p, b, U32(C().control, 0.6f), S(8));
    ImVec2 ia(p.x + S(8), p.y + S(10)), ib(b.x - S(8), b.y - S(8));

    float range = std::max(1e-6f, vmax - vmin);
    for (int g = 0; g < guideCount; ++g) {
        float v = (guides[g].value - vmin) / range;
        if (v < 0 || v > 1) continue;
        float y = ib.y - (ib.y - ia.y) * v;
        for (float x = ia.x; x < ib.x; x += S(8))   // dashed line
            dl->AddLine(ImVec2(x, y), ImVec2(std::min(x + S(4), ib.x), y), U32(guides[g].color, 0.55f), S(1));
        if (guides[g].label) {
            ImVec2 ts = ImGui::CalcTextSize(guides[g].label);
            dl->AddText(ImVec2(ib.x - ts.x - S(2), y - ts.y - S(1)), U32(guides[g].color, 0.85f), guides[g].label);
        }
    }

    if (count >= 2) {
        ImVector<ImVec2> pts; pts.resize(count);
        dl->PushClipRect(p, b, true);
        DrawSeries(dl, values, count, vmin, vmax, ia, ib, color, S(1.8f), pts.Data);
        dl->PopClipRect();

        if (hovered) {
            float mx = ImGui::GetIO().MousePos.x;
            int idx = std::clamp(int(std::lround((mx - ia.x) / (ib.x - ia.x) * (count - 1))), 0, count - 1);
            ImVec2 pt = pts[idx];
            dl->AddLine(ImVec2(pt.x, ia.y), ImVec2(pt.x, ib.y), U32(C().text, 0.25f), S(1));
            dl->AddCircleFilled(pt, S(4), U32(color), 16);
            dl->AddCircle(pt, S(4), U32(C().background, 1.0f), 16, S(1.5f));
            ImGui::SetTooltip("%.2f %s", values[idx], unit ? unit : "");
        }
    }
}

void StatTile(const char* label, const char* value, const char* unit, const float* spark, int sparkCount, const ImVec4* color) {
    float w = ImGui::GetContentRegionAvail().x;
    float h = S(spark ? 96.0f : 76.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, h));
    if (!ImGui::IsItemVisible()) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 b(p.x + w, p.y + h);
    dl->AddRectFilled(p, b, U32(C().surface), S(10));
    dl->AddRect(p, b, U32(C().border), S(10));

    ImVec4 accent = color ? *color : C().accent;
    dl->AddText(ImVec2(p.x + S(14), p.y + S(10)), U32(C().textMuted), label);
    ImFont* big = GetFont(Font::Large);
    float bigSize = big->FontSize;
    ImVec2 vs = big->CalcTextSizeA(bigSize, FLT_MAX, 0, value);
    float vy = p.y + S(10) + ImGui::GetTextLineHeight() + S(2);
    dl->AddText(big, bigSize, ImVec2(p.x + S(14), vy), U32(C().text), value);
    if (unit) dl->AddText(ImVec2(p.x + S(14) + vs.x + S(4), vy + vs.y - ImGui::GetTextLineHeight() - S(3)), U32(C().textMuted), unit);
    // Accent pip
    dl->AddCircleFilled(ImVec2(b.x - S(16), p.y + S(17)), S(3.5f), U32(accent), 12);

    if (spark && sparkCount > 1) {
        float mn = FLT_MAX, mx = -FLT_MAX;
        for (int i = 0; i < sparkCount; ++i) { mn = std::min(mn, spark[i]); mx = std::max(mx, spark[i]); }
        float pad = std::max(1e-3f, (mx - mn) * 0.15f);
        ImVector<ImVec2> pts; pts.resize(sparkCount);
        ImVec2 sa(p.x + S(1), b.y - S(26)), sb(b.x - S(1), b.y - S(4));
        dl->PushClipRect(p, b, true);
        DrawSeries(dl, spark, sparkCount, mn - pad, mx + pad, sa, sb, accent, S(1.4f), pts.Data);
        dl->PopClipRect();
    }
}

void Badge(const char* text, const ImVec4& color) {
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 pad(S(8), S(2));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 size(ts.x + pad.x * 2, ts.y + pad.y * 2);
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), U32(color, 0.16f), size.y);
    dl->AddText(ImVec2(p.x + pad.x, p.y + pad.y), U32(color), text);
}

void ProgressBar(float fraction, const ImVec2& sizeIn, const ImVec4* color, const char* overlay) {
    ImVec2 size(sizeIn.x > 0 ? sizeIn.x : ImGui::GetContentRegionAvail().x, sizeIn.y > 0 ? sizeIn.y : S(6));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(size);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float f = std::clamp(fraction, 0.0f, 1.0f);
    ImVec4 col = color ? *color : C().accent;
    dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), U32(C().control), size.y);
    if (f > 0) dl->AddRectFilled(p, ImVec2(p.x + std::max(size.y, size.x * f), p.y + size.y), U32(col), size.y);
    if (overlay) {
        ImVec2 ts = ImGui::CalcTextSize(overlay);
        dl->AddText(ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), U32(C().text), overlay);
    }
}

void KeyValue(const char* key, const char* fmt, ...) {
    char buf[512];
    va_list args; va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float lh = ImGui::GetTextLineHeight();
    ImGui::Dummy(ImVec2(w, lh));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(p, U32(C().textMuted), key);
    ImVec2 ts = ImGui::CalcTextSize(buf);
    dl->AddText(ImVec2(p.x + w - ts.x, p.y), U32(C().text), buf);
}

void TextMuted(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, C().textMuted);
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, C().textDisabled);
    ImGui::TextUnformatted(detail::GetFonts().iconsLoaded ? icons::Info.c_str() : "(?)");
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(S(320));
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void Spinner(const char* id, float radius, float thickness, const ImVec4& color) {
    (void)id;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float t = float(ImGui::GetTime());
    float start = t * 6.0f;
    float sweep = 1.5f + std::sin(t * 2.5f) * 1.0f;
    ImVec2 c(p.x + radius, p.y + radius);
    dl->PathArcTo(c, radius - thickness, start, start + sweep, 24);
    dl->PathStroke(U32(color), ImDrawFlags_None, thickness);
}

} // namespace blaze::ui
