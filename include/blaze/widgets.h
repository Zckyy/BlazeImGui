// =============================================================================
// blaze/widgets.h - Modern styled widgets that match the BlazeImGui theme.
//
// Prefer these over raw ImGui widgets inside panels for a consistent look.
// They follow ImGui conventions: return true when the value changed / clicked,
// and labels support "##id" suffixes.
//
// Layout pattern used by the built-in pages:
//
//     if (blaze::ui::BeginCard("Rendering", blaze::icons::Monitor)) {
//         blaze::ui::Toggle("Wireframe", &g_wireframe, "Draw all meshes as lines");
//         blaze::ui::SliderFloat("Exposure", &g_exposure, 0.f, 4.f, "%.2f");
//     }
//     blaze::ui::EndCard();   // ALWAYS call, even if BeginCard returned false
//
// "Setting rows" (Toggle, SliderFloat, SliderInt, Combo, ColorEdit, KeyBind)
// draw the label + optional description on the left and the control right-aligned,
// like the Windows 11 Settings app.
// =============================================================================
#pragma once
#include <imgui.h>
#include <string>

namespace blaze::ui {

// ---- Containers -----------------------------------------------------------------
bool BeginCard(const char* title, const char* icon = nullptr, const char* subtitle = nullptr);
void EndCard();
void SectionHeader(const char* text);             // Small caps-style group label
void PageHeader(const char* title, const char* subtitle = nullptr);
// Grid of equally sized columns for stat tiles. Call NextColumn() between items.
void BeginColumns(int count, const char* id);
void NextColumn();
void EndColumns();

// ---- Setting rows ---------------------------------------------------------------
bool Toggle(const char* label, bool* v, const char* description = nullptr);
bool SliderFloat(const char* label, float* v, float min, float max, const char* fmt = "%.2f", const char* description = nullptr);
bool SliderInt(const char* label, int* v, int min, int max, const char* fmt = "%d", const char* description = nullptr);
bool Combo(const char* label, int* current, const char* const items[], int count, const char* description = nullptr);
bool ColorEdit(const char* label, ImVec4* color, const char* description = nullptr);
bool KeyBind(const char* label, int* vk, const char* description = nullptr); // Win32 VK code
bool InputText(const char* label, std::string* str, const char* hint = nullptr, const char* description = nullptr);

// ---- Buttons ------------------------------------------------------------------
enum class ButtonKind { Primary, Secondary, Subtle, Danger };
bool Button(const char* label, ButtonKind kind = ButtonKind::Secondary, const ImVec2& size = ImVec2(0, 0));
bool IconButton(const char* icon, const char* tooltip = nullptr, bool active = false, float size = 0);
// Segmented control (e.g. Dark | Light | System). Returns true on change.
bool Segmented(const char* id, int* current, const char* const items[], int count, float width = 0);

// ---- Display --------------------------------------------------------------------
void StatTile(const char* label, const char* value, const char* unit = nullptr,
              const float* spark = nullptr, int sparkCount = 0, const ImVec4* color = nullptr);
void Badge(const char* text, const ImVec4& color);
void Sparkline(const char* id, const float* values, int count, float min, float max,
               const ImVec2& size, const ImVec4& color);
// Large frame-time style graph with hover readout and horizontal guide lines.
struct GraphGuide { float value; const char* label; ImVec4 color; };
void LineGraph(const char* id, const float* values, int count, float min, float max,
               const ImVec2& size, const ImVec4& color, const GraphGuide* guides = nullptr,
               int guideCount = 0, const char* unit = "ms");
void ProgressBar(float fraction, const ImVec2& size, const ImVec4* color = nullptr, const char* overlay = nullptr);
void KeyValue(const char* key, const char* fmt, ...);   // Muted key, right-aligned value
void TextMuted(const char* fmt, ...);
void HelpMarker(const char* text);
void Spinner(const char* id, float radius, float thickness, const ImVec4& color);

// ---- Fonts ----------------------------------------------------------------------
enum class Font { Regular, Semibold, Title, Large, Mono };
ImFont* GetFont(Font f);   // Falls back to default font if not loaded
void    PushFont(Font f);
void    PopFont();

const char* VirtualKeyName(int vk);

} // namespace blaze::ui
