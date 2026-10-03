// Built-in "Performance" page: frame timing, CPU scopes, counters, system stats.
#include "internal.h"

#include <algorithm>
#include <cstring>

namespace blaze::detail {
namespace {

bool  g_paused = false;
float g_frozen[perf::kHistory] = {};
int   g_targetIdx = 1;
const char* kTargets[] = { "30", "60", "120", "144", "165", "240" };
const float kTargetValues[] = { 30, 60, 120, 144, 165, 240 };

void Draw() {
    using namespace ui;
    const auto& c = theme::Colors();
    const auto& fs = perf::Frame();
    const auto& ss = perf::System();
    const float S = GetUIScale();
    const float target = perf::GetTargetFps();
    const float targetMs = 1000.0f / target;

    // ---- Stat tiles ----------------------------------------------------------
    static float fpsHist[perf::kHistory];
    for (int i = 0; i < perf::kHistory; ++i) fpsHist[i] = fs.history[i] > 0 ? 1000.0f / fs.history[i] : 0;
    ImVec4 fpsCol = fs.fps >= target * 0.95f ? c.success : fs.fps >= target * 0.5f ? c.warning : c.error;

    char v[32];
    BeginColumns(4, "##tiles");
    snprintf(v, sizeof v, "%.0f", fs.fps);
    StatTile("Frame rate", v, "fps", fpsHist + perf::kHistory - 90, 90, &fpsCol);
    NextColumn();
    snprintf(v, sizeof v, "%.2f", fs.frameMs);
    StatTile("Frame time", v, "ms", fs.history + perf::kHistory - 90, 90, &c.accent);
    NextColumn();
    snprintf(v, sizeof v, "%.0f", fs.low1PctFps);
    ImVec4 lowCol = fs.low1PctFps >= target * 0.8f ? c.info : c.warning;
    StatTile("1% low", v, "fps", nullptr, 0, &lowCol);
    NextColumn();
    snprintf(v, sizeof v, "%.0f", ss.workingSetMB);
    StatTile("Memory", v, "MB", nullptr, 0, &c.info);
    EndColumns();

    // ---- Frame time graph ---------------------------------------------------
    if (BeginCard("Frame time", icons::Chart, "Last 240 frames. Dashed lines mark frame budgets.")) {
        if (!g_paused) memcpy(g_frozen, fs.history, sizeof(g_frozen));
        float mx = 0;
        for (float x : g_frozen) mx = std::max(mx, x);
        mx = std::max(mx * 1.15f, targetMs * 1.6f);
        GraphGuide guides[] = {
            { targetMs, nullptr, c.success },
            { targetMs * 2.0f, nullptr, c.warning },
        };
        char l0[32], l1[32];
        snprintf(l0, sizeof l0, "%.0f fps", target);
        snprintf(l1, sizeof l1, "%.0f fps", target * 0.5f);
        guides[0].label = l0; guides[1].label = l1;
        LineGraph("##frametime", g_frozen, perf::kHistory, 0, mx, ImVec2(0, 150 * S), c.accent, guides, 2, "ms");

        ImGui::Dummy(ImVec2(0, 2 * S));
        auto metric = [&](const char* label, float value, const ImVec4& col) {
            TextMuted("%s", label);
            PushFont(Font::Semibold);
            ImGui::TextColored(col, "%.2f ms", value);
            PopFont();
        };
        BeginColumns(4, "##ftstats");
        metric("Average", fs.avgMs, c.text); NextColumn();
        metric("Best", fs.minMs, c.success); NextColumn();
        metric("Worst", fs.maxMs, fs.maxMs > targetMs * 2 ? c.warning : c.text); NextColumn();
        metric("Budget", targetMs, c.textMuted);
        EndColumns();

        if (Combo("Target frame rate", &g_targetIdx, kTargets, IM_ARRAYSIZE(kTargets), "Used for budget lines and status colours"))
            perf::SetTargetFps(kTargetValues[g_targetIdx]);
        Toggle("Freeze graph", &g_paused, "Pause the graph to inspect a spike");
    }
    EndCard();

    // ---- CPU scopes ---------------------------------------------------------
    auto scopes = perf::Scopes();
    if (BeginCard("CPU scopes", icons::Clock, "Wrap code in BLAZE_PROFILE_SCOPE(\"Name\") to measure it")) {
        if (scopes.empty()) {
            TextMuted("No scopes recorded yet. Example:");
            PushFont(Font::Mono);
            ImGui::TextColored(c.accent, "void Physics::Step() { BLAZE_PROFILE_SCOPE(\"Physics\"); ... }");
            PopFont();
        } else if (ImGui::BeginTable("##scopes", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed, 70 * S);
            ImGui::TableSetupColumn("Peak", ImGuiTableColumnFlags_WidthFixed, 70 * S);
            ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 50 * S);
            ImGui::TableSetupColumn("Share of frame", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, c.textMuted);
            ImGui::TableHeadersRow();
            ImGui::PopStyleColor();
            for (auto& s : scopes) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(s.name.c_str());
                ImGui::TableNextColumn(); ImGui::Text("%.3f", s.avgMs);
                ImGui::TableNextColumn();
                ImGui::TextColored(s.maxMs > targetMs * 0.5f ? c.warning : c.textMuted, "%.3f", s.maxMs);
                ImGui::TableNextColumn(); ImGui::TextColored(c.textMuted, "%d", s.calls);
                ImGui::TableNextColumn();
                float frac = fs.avgMs > 0 ? s.avgMs / fs.avgMs : 0;
                char pct[16]; snprintf(pct, sizeof pct, "%.1f%%", frac * 100);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetTextLineHeight() * 0.35f);
                ProgressBar(frac, ImVec2(-1, 6 * S));
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s of average frame time", pct);
            }
            ImGui::EndTable();
        }
        if (!scopes.empty() && Button("Reset peaks", ButtonKind::Subtle)) perf::ResetPeaks();
    }
    EndCard();

    // ---- Counters ----------------------------------------------------------
    auto counters = perf::Counters();
    if (!counters.empty()) {
        if (BeginCard("Counters", icons::List, "Values from blaze::perf::SetCounter()")) {
            BeginColumns(3, "##counters");
            for (size_t i = 0; i < counters.size(); ++i) {
                if (i) NextColumn();
                auto& k = counters[i];
                char buf[32];
                if (k.value == double(long long(k.value))) snprintf(buf, sizeof buf, "%lld", (long long)k.value);
                else snprintf(buf, sizeof buf, "%.2f", k.value);
                StatTile(k.name.c_str(), buf, nullptr, k.history + perf::kHistory - 120, 120, &c.accent);
            }
            EndColumns();
        }
        EndCard();
    }

    // ---- System ------------------------------------------------------------
    if (BeginCard("System", icons::Monitor)) {
        BeginColumns(2, "##sys");
        KeyValue("GPU", "%s", ss.gpuName.empty() ? "Unknown" : ss.gpuName.c_str());
        KeyValue("Logical CPU cores", "%d", ss.cpuCores);
        KeyValue("Process CPU", "%.1f %%", ss.cpuProcessPct);
        ProgressBar(ss.cpuProcessPct / 100.0f, ImVec2(-1, 5 * S));
        NextColumn();
        KeyValue("Working set", "%.0f MB", ss.workingSetMB);
        KeyValue("Private bytes", "%.0f MB", ss.privateMB);
        KeyValue("System RAM", "%.0f %% of %.1f GB", ss.systemUsedPct, ss.systemTotalGB);
        ProgressBar(float(ss.systemUsedPct / 100.0), ImVec2(-1, 5 * S), ss.systemUsedPct > 85 ? &c.warning : nullptr);
        EndColumns();
        if (ss.vramBudgetMB > 0) {
            ImGui::Dummy(ImVec2(0, 2 * S));
            KeyValue("Video memory", "%.0f / %.0f MB", ss.vramUsedMB, ss.vramBudgetMB);
            float f = float(ss.vramUsedMB / ss.vramBudgetMB);
            ProgressBar(f, ImVec2(-1, 5 * S), f > 0.9f ? &c.error : f > 0.75f ? &c.warning : nullptr);
        }
    }
    EndCard();
}

} // namespace

void RegisterPerformancePanel() {
    Panel p;
    p.id = "blaze.performance";
    p.title = "Performance";
    p.subtitle = "Frame timing, CPU scopes and system resources";
    p.category = "Overview";
    p.icon = icons::Speed;
    p.order = 0;
    p.draw = Draw;
    RegisterPanel(p);
    // Keep the combo in sync with the default target.
    for (int i = 0; i < IM_ARRAYSIZE(kTargetValues); ++i)
        if (kTargetValues[i] == perf::GetTargetFps()) g_targetIdx = i;
}

} // namespace blaze::detail
