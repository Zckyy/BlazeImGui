// Built-in "Configs" page: profile management, import/export, variable browser.
#include "internal.h"

#include <algorithm>

namespace blaze::detail {
namespace {

char        g_newName[64] = "";
std::string g_renaming;
char        g_renameBuf[64] = "";
std::string g_confirmDelete;
char        g_varFilter[96] = "";

const char* TypeName(config::VarType t) {
    switch (t) {
    case config::VarType::Bool:   return "bool";
    case config::VarType::Int:    return "int";
    case config::VarType::Float:  return "float";
    case config::VarType::String: return "string";
    case config::VarType::Color:  return "color";
    case config::VarType::Key:    return "key";
    }
    return "?";
}

void ProfileRow(const std::string& name, bool active) {
    using namespace ui;
    const auto& c = theme::Colors();
    const float S = GetUIScale();
    ImGui::PushID(name.c_str());

    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float h = 44 * S;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool hovered = ImGui::IsMouseHoveringRect(p, ImVec2(p.x + w, p.y + h)) && ImGui::IsWindowHovered();
    if (active) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::U32(c.accent, 0.10f), 8 * S);
    else if (hovered) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), theme::U32(c.surfaceHover), 8 * S);

    float lh = ImGui::GetTextLineHeight();
    float x = p.x + 12 * S;
    if (GetFonts().iconsLoaded) {
        dl->AddText(ImVec2(x, p.y + (h - lh) * 0.5f), theme::U32(active ? c.accent : c.textMuted), icons::Document);
        x += 26 * S;
    }

    if (g_renaming == name) {
        ImGui::SetCursorScreenPos(ImVec2(x, p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::SetNextItemWidth(200 * S);
        if (ImGui::IsWindowAppearing() || ImGui::GetActiveID() == 0) ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##rename", g_renameBuf, sizeof g_renameBuf, ImGuiInputTextFlags_EnterReturnsTrue)) {
            if (config::RenameProfile(name, g_renameBuf)) Notify("Profile renamed", ToastKind::Success);
            g_renaming.clear();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g_renaming.clear();
    } else {
        ImFont* f = GetFont(active ? Font::Semibold : Font::Regular);
        dl->AddText(f, f->FontSize, ImVec2(x, p.y + (h - lh) * 0.5f), theme::U32(c.text), name.c_str());
        if (active) {
            ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0, name.c_str());
            ImGui::SetCursorScreenPos(ImVec2(x + ts.x + 10 * S, p.y + (h - lh) * 0.5f - 2 * S));
            Badge(config::IsDirty() ? "Active - unsaved" : "Active", config::IsDirty() ? c.warning : c.accent);
        }
    }

    // Right-aligned actions
    float btn = ImGui::GetFrameHeight() + 4 * S;
    float bx = p.x + w - 8 * S;
    auto place = [&](float width) { bx -= width; ImGui::SetCursorScreenPos(ImVec2(bx, p.y + (h - btn) * 0.5f)); bx -= 4 * S; };

    if (g_confirmDelete == name) {
        place(70 * S);
        if (Button("Cancel", ButtonKind::Subtle, ImVec2(70 * S, btn))) g_confirmDelete.clear();
        place(110 * S);
        if (Button("Confirm delete", ButtonKind::Danger, ImVec2(110 * S, btn))) {
            config::DeleteProfile(name);
            Notify(("Deleted profile '" + name + "'").c_str(), ToastKind::Warning);
            g_confirmDelete.clear();
        }
    } else {
        place(btn);
        if (IconButton(icons::Trash, "Delete")) g_confirmDelete = name;
        place(btn);
        if (IconButton(icons::Edit, "Rename")) { g_renaming = name; strncpy_s(g_renameBuf, name.c_str(), _TRUNCATE); }
        place(btn);
        if (IconButton(icons::Copy, "Duplicate")) {
            std::string copy = name + " copy";
            if (config::DuplicateProfile(name, copy)) Notify(("Created '" + copy + "'").c_str(), ToastKind::Success);
        }
        if (!active) {
            place(70 * S);
            if (Button("Load", ButtonKind::Secondary, ImVec2(70 * S, btn))) {
                if (config::IsDirty() && config::GetAutoSave()) config::SaveProfile();
                if (config::LoadProfile(name)) Notify(("Loaded profile '" + name + "'").c_str(), ToastKind::Success);
            }
        }
    }

    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
    ImGui::PopID();
}

void Draw() {
    using namespace ui;
    const auto& c = theme::Colors();
    const float S = GetUIScale();
    auto& st = Settings();

    // ---- Active profile actions ----
    if (BeginCard("Current profile", icons::Save, config::Directory().c_str())) {
        if (Button(config::IsDirty() ? "Save changes" : "Save", ButtonKind::Primary)) {
            if (config::SaveProfile()) Notify("Profile saved", ToastKind::Success);
        }
        ImGui::SameLine();
        if (Button("Revert")) {
            if (config::LoadProfile(config::ActiveProfile())) Notify("Reverted to last saved state", ToastKind::Info);
        }
        ImGui::SameLine();
        if (Button("Reset to defaults")) {
            config::ResetToDefaults();
            Notify("Values reset to defaults (not saved yet)", ToastKind::Warning);
        }
        ImGui::SameLine();
        if (Button("Open folder", ButtonKind::Subtle)) config::OpenDirectoryInExplorer();
        ImGui::Dummy(ImVec2(0, 2 * S));
        Toggle("Auto-save", &st.autoSave, "Write changes to the active profile automatically (1 s after the last edit)");
    }
    EndCard();

    // ---- Profiles list ----
    if (BeginCard("Profiles", icons::Folder, "Each profile is a JSON file in the profiles folder")) {
        auto profiles = config::ListProfiles();
        for (auto& name : profiles) ProfileRow(name, name == config::ActiveProfile());
        if (profiles.empty()) TextMuted("No profiles saved yet.");

        ImGui::Dummy(ImVec2(0, 4 * S));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * S, 7 * S));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_Border, c.border);
        ImGui::SetNextItemWidth(std::max(120 * S, ImGui::GetContentRegionAvail().x - 180 * S));
        bool enter = ImGui::InputTextWithHint("##newprofile", "New profile name", g_newName, sizeof g_newName,
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);
        ImGui::SameLine();
        if ((Button("Save current as new", ButtonKind::Primary, ImVec2(-1, 0)) || enter) && g_newName[0]) {
            if (config::SaveProfile(g_newName)) Notify(("Saved as '" + std::string(g_newName) + "'").c_str(), ToastKind::Success);
            g_newName[0] = 0;
        }
    }
    EndCard();

    // ---- Share ----
    if (BeginCard("Share", icons::Upload, "Copy the active profile as JSON, or paste one to apply it")) {
        if (Button("Export to clipboard")) {
            ImGui::SetClipboardText(config::ExportProfileJson().c_str());
            Notify("Profile JSON copied", ToastKind::Success);
        }
        ImGui::SameLine();
        if (Button("Import from clipboard")) {
            const char* clip = ImGui::GetClipboardText();
            if (clip && config::ImportProfileJson(clip)) Notify("Profile imported", ToastKind::Success);
            else Notify("Clipboard does not contain a valid profile", ToastKind::Error);
        }
    }
    EndCard();

    // ---- Variables ----
    if (BeginCard("Variables", icons::List, "Everything registered with blaze::config::Bind()")) {
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##varfilter", "Filter by key...", g_varFilter, sizeof g_varFilter);
        if (ImGui::BeginTable("##vars", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 2.2f);
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 56 * S);
            ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthFixed, 64 * S);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.6f);
            ImGui::PushStyleColor(ImGuiCol_Text, c.textMuted);
            ImGui::TableHeadersRow();
            ImGui::PopStyleColor();
            std::string f = g_varFilter;
            for (auto& v : config::ListVars()) {
                if (!f.empty() && v.key.find(f) == std::string::npos) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                PushFont(Font::Mono); ImGui::TextUnformatted(v.key.c_str()); PopFont();
                ImGui::TableNextColumn(); ImGui::TextColored(c.textMuted, "%s", TypeName(v.type));
                ImGui::TableNextColumn();
                ImGui::TextColored(v.scope == config::Scope::Global ? c.info : c.textMuted, "%s",
                                   v.scope == config::Scope::Global ? "global" : "profile");
                ImGui::TableNextColumn();
                PushFont(Font::Mono); ImGui::TextUnformatted(v.valueJson.c_str()); PopFont();
            }
            ImGui::EndTable();
        }
    }
    EndCard();
}

} // namespace

void RegisterConfigPanel() {
    Panel p;
    p.id = "blaze.configs";
    p.title = "Configs";
    p.subtitle = "Save, load and share configuration profiles";
    p.category = "System";
    p.icon = icons::Save;
    p.order = 10;
    p.draw = Draw;
    RegisterPanel(p);
}

} // namespace blaze::detail
