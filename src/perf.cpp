#include "internal.h"

#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>

namespace blaze::perf {
namespace {

struct ScopeAccum {
    double frameMs = 0; int frameCalls = 0;    // accumulating this frame
    float  lastMs = 0;  int lastCalls = 0;     // previous frame
    float  avgMs = 0;
    float  peakMs = 0, peakWindow = 0;
    bool   seen = false;
};
struct CounterData { double value = 0; float history[kHistory] = {}; };

std::mutex                              g_mutex;
std::map<std::string, ScopeAccum>       g_scopes;
std::map<std::string, CounterData>      g_counters;

FrameStats    g_frame;
SystemStats   g_system;
float         g_targetFps = 60.0f;
LARGE_INTEGER g_freq{}, g_last{};
float         g_lowTimer = 0, g_sysTimer = 0, g_peakTimer = 0;
IDXGIAdapter3* g_adapter3 = nullptr;
ULONGLONG     g_lastProcTime = 0, g_lastWallTime = 0;

int64_t Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

ULONGLONG FileTimeToU64(const FILETIME& ft) { return (ULONGLONG(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

void PushHistory(float* h, float v) {
    memmove(h, h + 1, sizeof(float) * (kHistory - 1));
    h[kHistory - 1] = v;
}

void SampleSystem() {
    HANDLE proc = GetCurrentProcess();

    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(proc, (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        g_system.workingSetMB = double(pmc.WorkingSetSize) / (1024.0 * 1024.0);
        g_system.privateMB    = double(pmc.PrivateUsage) / (1024.0 * 1024.0);
    }

    MEMORYSTATUSEX ms{ sizeof(ms) };
    if (GlobalMemoryStatusEx(&ms)) {
        g_system.systemUsedPct = double(ms.dwMemoryLoad);
        g_system.systemTotalGB = double(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
    }

    FILETIME create, exit, kernel, user, now;
    if (GetProcessTimes(proc, &create, &exit, &kernel, &user)) {
        GetSystemTimeAsFileTime(&now);
        ULONGLONG proc100ns = FileTimeToU64(kernel) + FileTimeToU64(user);
        ULONGLONG wall100ns = FileTimeToU64(now);
        if (g_lastWallTime && wall100ns > g_lastWallTime) {
            double pct = double(proc100ns - g_lastProcTime) / double(wall100ns - g_lastWallTime) * 100.0;
            g_system.cpuProcessPct = float(std::clamp(pct / std::max(1, g_system.cpuCores), 0.0, 100.0));
        }
        g_lastProcTime = proc100ns;
        g_lastWallTime = wall100ns;
    }

    if (g_adapter3) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            g_system.vramUsedMB   = double(info.CurrentUsage) / (1024.0 * 1024.0);
            g_system.vramBudgetMB = double(info.Budget) / (1024.0 * 1024.0);
        }
    }
}

} // namespace

const FrameStats&  Frame()  { return g_frame; }
const SystemStats& System() { return g_system; }

std::vector<ScopeStats> Scopes() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<ScopeStats> out;
    for (auto& [name, s] : g_scopes)
        if (s.seen) out.push_back({ name, s.lastMs, s.avgMs, s.peakMs, s.lastCalls });
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.avgMs > b.avgMs; });
    return out;
}

std::vector<Counter> Counters() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::vector<Counter> out;
    for (auto& [name, c] : g_counters) {
        Counter k{ name, c.value, {} };
        memcpy(k.history, c.history, sizeof(k.history));
        out.push_back(std::move(k));
    }
    return out;
}

void SetCounter(const char* name, double value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_counters[name].value = value;
}

void AddScopeTime(const char* name, double ms) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto& s = g_scopes[name];
    s.frameMs += ms;
    s.frameCalls++;
}

void ResetPeaks() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& [n, s] : g_scopes) s.peakMs = s.peakWindow = 0;
}

void  SetTargetFps(float fps) { g_targetFps = std::max(1.0f, fps); }
float GetTargetFps() { return g_targetFps; }

ScopeTimer::ScopeTimer(const char* name) : name_(name), start_(Now()) {}
ScopeTimer::~ScopeTimer() {
    double ms = double(Now() - start_) * 1000.0 / double(g_freq.QuadPart ? g_freq.QuadPart : 1);
    AddScopeTime(name_, ms);
}

void Init(void* adapter) {
    QueryPerformanceFrequency(&g_freq);
    g_last.QuadPart = Now();
    SYSTEM_INFO si; GetSystemInfo(&si);
    g_system.cpuCores = int(si.dwNumberOfProcessors);

    if (adapter) {
        auto* a = static_cast<IDXGIAdapter*>(adapter);
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(a->GetDesc(&desc))) {
            char name[256];
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            g_system.gpuName = name;
        }
        a->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&g_adapter3);
    }
    SampleSystem();
}

void Shutdown() {
    if (g_adapter3) { g_adapter3->Release(); g_adapter3 = nullptr; }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_scopes.clear();
    g_counters.clear();
}

void BeginFrame() {
    int64_t now = Now();
    float ms = float(double(now - g_last.QuadPart) * 1000.0 / double(g_freq.QuadPart));
    g_last.QuadPart = now;
    ms = std::clamp(ms, 0.0f, 1000.0f);
    float dt = ms / 1000.0f;

    g_frame.frameMs = ms;
    PushHistory(g_frame.history, ms);

    float sum = 0, mn = 1e9f, mx = 0; int n = 0;
    for (float v : g_frame.history) {
        if (v <= 0) continue;
        sum += v; mn = std::min(mn, v); mx = std::max(mx, v); ++n;
    }
    g_frame.avgMs = n ? sum / n : 0;
    g_frame.minMs = n ? mn : 0;
    g_frame.maxMs = mx;
    float instFps = ms > 0 ? 1000.0f / ms : 0;
    g_frame.fps = g_frame.fps <= 0 ? instFps : g_frame.fps + (instFps - g_frame.fps) * std::min(1.0f, dt * 4.0f);

    if ((g_lowTimer -= dt) <= 0) {
        g_lowTimer = 0.25f;
        float sorted[kHistory]; int count = 0;
        for (float v : g_frame.history) if (v > 0) sorted[count++] = v;
        if (count) {
            std::sort(sorted, sorted + count, std::greater<float>());
            int k = std::max(1, count / 100);
            float worst = 0;
            for (int i = 0; i < k; ++i) worst += sorted[i];
            g_frame.low1PctFps = 1000.0f / (worst / k);
        }
    }

    if ((g_sysTimer -= dt) <= 0) { g_sysTimer = 0.5f; SampleSystem(); }

    std::lock_guard<std::mutex> lock(g_mutex);
    bool resetPeaks = (g_peakTimer -= dt) <= 0;
    if (resetPeaks) g_peakTimer = 2.0f;
    for (auto& [name, s] : g_scopes) {
        s.lastMs = float(s.frameMs);
        s.lastCalls = s.frameCalls;
        if (s.frameCalls) s.seen = true;
        s.avgMs = s.avgMs + (s.lastMs - s.avgMs) * 0.08f;
        s.peakWindow = std::max(s.peakWindow, s.lastMs);
        if (resetPeaks) { s.peakMs = s.peakWindow; s.peakWindow = 0; }
        else s.peakMs = std::max(s.peakMs, s.lastMs);
        s.frameMs = 0; s.frameCalls = 0;
    }
    for (auto& [name, c] : g_counters) PushHistory(c.history, float(c.value));
}

} // namespace blaze::perf
