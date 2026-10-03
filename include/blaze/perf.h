// =============================================================================
// blaze/perf.h - Frame timing, CPU scope profiling and custom counters.
//
// Frame timing and process stats (CPU %, memory, VRAM) are collected
// automatically. Add your own data from game code:
//
//     void Physics::Step() {
//         BLAZE_PROFILE_SCOPE("Physics");        // shows up in Performance > Scopes
//         ...
//     }
//     blaze::perf::SetCounter("Entities", (double)world.size());
//     blaze::perf::SetCounter("Draw calls", drawCalls);
//
// Thread safety: ScopeTimer, SetCounter and AddScopeTime may be called from any
// thread. Everything else is main/render-thread only.
// =============================================================================
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace blaze::perf {

constexpr int kHistory = 240; // Samples kept for graphs

struct FrameStats {
    float fps = 0;           // Smoothed
    float frameMs = 0;       // Last frame
    float avgMs = 0;         // Over history window
    float minMs = 0;
    float maxMs = 0;
    float low1PctFps = 0;    // 1% low FPS
    float history[kHistory] = {};  // Frame times (ms), oldest first
};

struct SystemStats {
    float  cpuProcessPct = 0;  // This process, 0..100 (normalized across cores)
    double workingSetMB = 0;   // Physical memory used by this process
    double privateMB = 0;      // Committed private bytes
    double systemUsedPct = 0;  // System RAM load
    double systemTotalGB = 0;
    double vramUsedMB = 0;     // From DXGI (0 if unavailable)
    double vramBudgetMB = 0;
    std::string gpuName;
    int    cpuCores = 0;
};

struct ScopeStats {
    std::string name;
    float lastMs = 0;    // Total time in last completed frame
    float avgMs = 0;     // Exponential moving average
    float maxMs = 0;     // Peak over last ~2s
    int   calls = 0;     // Calls in last completed frame
};

struct Counter { std::string name; double value; float history[kHistory]; };

const FrameStats&       Frame();
const SystemStats&      System();
std::vector<ScopeStats> Scopes();
std::vector<Counter>    Counters();

void SetCounter(const char* name, double value);
void AddScopeTime(const char* name, double ms);
void ResetPeaks();

// Frame pacing target used for graph guide lines and the status color.
void  SetTargetFps(float fps);
float GetTargetFps();

class ScopeTimer {
public:
    explicit ScopeTimer(const char* name);
    ~ScopeTimer();
    ScopeTimer(const ScopeTimer&) = delete;
    ScopeTimer& operator=(const ScopeTimer&) = delete;
private:
    const char* name_;
    int64_t     start_;
};

// Internal -----------------------------------------------------------------------
void Init(void* dxgiAdapterOrNull);
void BeginFrame();  // Called by blaze::NewFrame()
void Shutdown();

} // namespace blaze::perf

#define BLAZE_CONCAT_INNER(a, b) a##b
#define BLAZE_CONCAT(a, b) BLAZE_CONCAT_INNER(a, b)
#define BLAZE_PROFILE_SCOPE(name) ::blaze::perf::ScopeTimer BLAZE_CONCAT(_blazeScope, __LINE__)(name)
