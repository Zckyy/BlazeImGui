// Built-in "Debug" page: log console + command line, watches, command list, ImGui tools.
#include "internal.h"

#include <algorithm>
#include <map>

namespace blaze::detail {
namespace {

int         g_tab = 0;
bool        g_levels[4] = { false, true, true, true };  // Trace hidden by default
bool        g_autoScroll = true;
char        g_filter[128] = "";
char        g_input[256] = "";
int         g_historyPos = -1;
bool        g_refocus = false;
bool        g_showDemo = false, g_showMetrics = false, g_showStyle = false;

ImVec4 LevelColor(LogLevel l) {
    const auto& c = theme::Colors();
    switch (l) {
    case LogLevel::Trace: return c.textDisabled;
    case LogLevel::Info:  return c.info;
    case LogLevel::Warn:  return c.warning;
    case LogLevel::Error: return c.error;
    }
    return c.text;
}

const char* LevelName(LogLevel l) {
    static const char* n[] = { "TRACE", "INFO", "WARN", "ERROR" };
    return n[int(l)];
}

bool ContainsI(const std::string& hay, const char* needle) {
    if (!*needle) return true;
    auto it = std::search(hay.begin(), hay.end(), needle, needle + strlen(needle),
        [](char a, char b) { return tolower((unsigned char)a) == tolower((unsigned char)b); });
    return it != hay.end();
}

int InputCallback(ImGuiInputTextCallbackData* data) {
    const auto& hist = debug::History();
    if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory && !hist.empty()) {
        int prev = g_historyPos;
        if (data->EventKey == ImGuiKey_UpArrow) {
            g_historyPos = g_historyPos < 0 ? int(hist.size()) - 1 : std::max(0, g_historyPos - 1);
        } else if (data->EventKey == ImGuiKey_DownArrow && g_historyPos >= 0) {
            if (++g_historyPos >= int(hist.size())) g_historyPos = -1;
        }
        if (prev != g_historyPos) {
            data->DeleteChars(0, data->BufTextLen);
            if (g_historyPos >= 0) data->InsertChars(0, hist[size_t(g_historyPos)].c_str());
        }
    } else if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
        std::string word(data->Buf, size_t(data->BufTextLen));
        std::vector<std::string> matches;
        for (auto& c : debug::ListCommands())
            if (_strnicmp(c.name.c_str(), word.c_str(), word.size()) == 0) matches.push_back(c.name);
        if (matches.size() == 1) {
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, (matches[0] + " ").c_str());
        } else if (matches.size() > 1) {
            // Complete the common prefix and list candidates.
            std::string prefix = matches[0];
            for (auto& m : matches)
                while (!prefix.empty() && _strnicmp(prefix.c_str(), m.c_str(), prefix.size()) != 0) prefix.pop_back();
            data->DeleteChars(0, data->BufTextLen);
            data->InsertChars(0, prefix.c_str());
            std::string all;
            for (auto& m : matches) all += m + "  ";
            Log::Write(LogLevel::Trace, "console", "%s", all.c_str());
        }
    }
    return 0;
}

void DrawConsole() {
    using namespace ui;
    const auto& c = theme::Colors();
    const float S = GetUIScale();

    // ---- Toolbar ----
    const char* names[] = { "Trace", "Info", "Warn", "Error" };
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine(0, 4 * S);
        ImVec4 col = LevelColor(LogLevel(i));
        ImGui::PushStyleColor(ImGuiCol_Button, g_levels[i] ? theme::WithAlpha(col, 0.18f) : c.control);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::WithAlpha(col, 0.28f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::WithAlpha(col, 0.36f));
        ImGui::PushStyleColor(ImGuiCol_Text, g_levels[i] ? col : c.textMuted);
        if (ImGui::Button(names[i])) g_levels[i] = !g_levels[i];
        ImGui::PopStyleColor(4);
    }
    ImGui::SameLine(0, 12 * S);
    float right = 3 * (ImGui::GetFrameHeight() + 8 * S);
    ImGui::SetNextItemWidth(std::max(80 * S, ImGui::GetContentRegionAvail().x - right));
    ImGui::InputTextWithHint("##filter", "Filter messages...", g_filter, sizeof g_filter);
    ImGui::SameLine();
    if (IconButton(icons::Pin, g_autoScroll ? "Auto-scroll: on" : "Auto-scroll: off", g_autoScroll)) g_autoScroll = !g_autoScroll;
    ImGui::SameLine();
    if (IconButton(icons::Copy, "Copy visible log to clipboard")) {
        std::string all;
        for (auto& e : Log::Snapshot())
            if (g_levels[int(e.level)] && (ContainsI(e.message, g_filter) || ContainsI(e.category, g_filter)))
                all += debug::Fmt("[%8.3f] [%s] [%s] %s\n", e.time, LevelName(e.level), e.category.c_str(), e.message.c_str());
        ImGui::SetClipboardText(all.c_str());
        Notify("Log copied to clipboard", ToastKind::Success);
    }
    ImGui::SameLine();
    if (IconButton(icons::Trash, "Clear log")) Log::Clear();

    // ---- Log view ----
    auto entries = Log::Snapshot();
    std::vector<const LogEntry*> visible;
    visible.reserve(entries.size());
    for (auto& e : entries)
        if (g_levels[int(e.level)] && (ContainsI(e.message, g_filter) || ContainsI(e.category, g_filter)))
            visible.push_back(&e);

    float inputH = ImGui::GetFrameHeight() + 16 * S;
    float h = std::max(200 * S, ImGui::GetContentRegionAvail().y - inputH);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::IsDark() ? ImVec4(0, 0, 0, 0.22f) : ImVec4(1, 1, 1, 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * S, 8 * S));
    ImGui::BeginChild("##log", ImVec2(0, h), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    PushFont(Font::Mono);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6 * S, 3 * S));
    ImGuiListClipper clipper;
    clipper.Begin(int(visible.size()));
    while (clipper.Step()) {
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const LogEntry& e = *visible[size_t(i)];
            ImGui::TextColored(c.textDisabled, "%8.3f", e.time);
            ImGui::SameLine();
            ImGui::TextColored(LevelColor(e.level), "%-5s", LevelName(e.level));
            ImGui::SameLine();
            ImGui::TextColored(c.textMuted, "%-8s", e.category.c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, e.level == LogLevel::Error ? c.error : e.level == LogLevel::Warn ? c.warning : c.text);
            ImGui::TextUnformatted(e.message.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                ImGui::SetClipboardText(e.message.c_str());
                Notify("Message copied", ToastKind::Info, 1.5f);
            }
        }
    }
    if (visible.empty()) ImGui::TextColored(c.textDisabled, "No messages");
    ImGui::PopStyleVar();
    PopFont();
    if (g_autoScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40 * S) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    // ---- Command line ----
    ImGui::Dummy(ImVec2(0, 2 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12 * S, 8 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, c.border);
    PushFont(Font::Mono);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (g_refocus) { ImGui::SetKeyboardFocusHere(); g_refocus = false; }
    if (ImGui::InputTextWithHint("##cmd", "> Type a command (Tab completes, Up/Down history, 'help' lists all)", g_input, sizeof g_input,
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory | ImGuiInputTextFlags_CallbackCompletion,
            InputCallback)) {
        if (g_input[0]) debug::Execute(g_input);
        g_input[0] = 0;
        g_historyPos = -1;
        g_refocus = true;
        g_autoScroll = true;
    }
    PopFont();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

void DrawWatches() {
    using namespace ui;
    const auto& c = theme::Colors();
    const auto& watches = debug::Watches();
    if (watches.empty()) {
        if (BeginCard("No watches", icons::View, "Register live values from code")) {
            PushFont(Font::Mono);
            ImGui::TextColored(c.accent, "blaze::debug::Watch(\"Player pos\", [] { return blaze::debug::Fmt(\"%%.1f, %%.1f\", x, y); });");
            PopFont();
        }
        EndCard();
        return;
    }
    std::map<std::string, std::vector<const debug::WatchInfo*>> groups;
    for (auto& w : watches) groups[w.group].push_back(&w);
    for (auto& [group, items] : groups) {
        if (BeginCard(group.c_str(), icons::View)) {
            for (auto* w : items) {
                std::string val = w->getter ? w->getter() : std::string();
                PushFont(Font::Regular);
                KeyValue(w->label.c_str(), "%s", val.c_str());
                PopFont();
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) ImGui::SetClipboardText(val.c_str());
            }
        }
        EndCard();
    }
}

void DrawCommands() {
    using namespace ui;
    const auto& c = theme::Colors();
    const float S = GetUIScale();
    if (BeginCard("Registered commands", icons::Console, "Run from the console or click Run (no arguments)")) {
        const std::string& q = SearchQuery();
        if (ImGui::BeginTable("##cmds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, 170 * S);
            ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 64 * S);
            for (auto& cmd : debug::ListCommands()) {
                if (!q.empty() && !ContainsI(cmd.name, q.c_str())) continue;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                PushFont(Font::Mono);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(c.accent, "%s", cmd.name.c_str());
                PopFont();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(c.textMuted, "%s", cmd.help.c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(cmd.name.c_str());
                if (Button("Run", ButtonKind::Subtle, ImVec2(56 * S, 0))) { debug::Execute(cmd.name); g_tab = 0; }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    EndCard();
}

void DrawTools() {
    using namespace ui;
    if (BeginCard("Dear ImGui tools", icons::Code)) {
        Toggle("Demo window", &g_showDemo, "Reference for every built-in ImGui widget");
        Toggle("Metrics / debugger", &g_showMetrics, "Inspect windows, draw calls and internal state");
        Toggle("Style editor", &g_showStyle, "Tweak raw ImGui style values live");
    }
    EndCard();
    if (BeginCard("State snapshot", icons::Download, "JSON with performance, config variables, watches and recent log")) {
        if (Button("Copy state JSON", ButtonKind::Primary)) {
            ImGui::SetClipboardText(DumpStateJson().c_str());
            Notify("State JSON copied to clipboard", ToastKind::Success);
        }
        ImGui::SameLine();
        if (Button("Write state_dump.json")) {
            debug::Execute("blaze.dump");
            Notify("State written to config folder", ToastKind::Success);
        }
    }
    EndCard();
}

void Draw() {
    const char* tabs[] = { "Console", "Watches", "Commands", "Tools" };
    ui::Segmented("##debugtabs", &g_tab, tabs, 4, 0);
    ImGui::Dummy(ImVec2(0, 4 * GetUIScale()));
    switch (g_tab) {
    case 0: DrawConsole(); break;
    case 1: DrawWatches(); break;
    case 2: DrawCommands(); break;
    case 3: DrawTools(); break;
    }
}

} // namespace

void RegisterDebugPanel() {
    Panel p;
    p.id = "blaze.debug";
    p.title = "Debug";
    p.subtitle = "Log console, commands and live watches";
    p.category = "Debug";
    p.icon = icons::Bug;
    p.order = 0;
    p.draw = Draw;
    RegisterPanel(p);
}

// ImGui tool windows are drawn outside the menu window so they can be moved freely.
// Called every frame from blaze::DrawUI().
void DrawDebugToolWindows() {
    if (!IsMenuOpen()) return;
    if (g_showDemo) ImGui::ShowDemoWindow(&g_showDemo);
    if (g_showMetrics) ImGui::ShowMetricsWindow(&g_showMetrics);
    if (g_showStyle) {
        ImGui::Begin("Style editor", &g_showStyle);
        ImGui::ShowStyleEditor();
        ImGui::End();
    }
}

} // namespace blaze::detail
