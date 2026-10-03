// =============================================================================
// blaze/debug.h - Log console, console commands and variable watches.
//
//     blaze::Log::Info("Loaded level %s in %.1f ms", name, ms);
//     blaze::Log::Warn("Missing texture: %s", path);
//
//     blaze::debug::RegisterCommand("teleport", "teleport <x> <y> <z>",
//         [](const blaze::debug::Args& a) {
//             if (a.size() < 4) return blaze::Log::Error("usage: teleport x y z");
//             player.pos = { std::stof(a[1]), std::stof(a[2]), std::stof(a[3]) };
//         });
//
//     blaze::debug::Watch("Player position", [] {
//         return blaze::debug::Fmt("%.2f, %.2f, %.2f", p.x, p.y, p.z);
//     });
//
// Logging is thread-safe. Commands and watches run on the render thread.
// =============================================================================
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace blaze {

enum class LogLevel : int { Trace = 0, Info, Warn, Error };

struct LogEntry {
    LogLevel    level;
    double      time;      // Seconds since blaze::Initialize()
    std::string category;  // Optional, e.g. "net", "ai". Empty = "app"
    std::string message;
};

namespace Log {
    void Write(LogLevel level, const char* category, const char* fmt, ...);
    void Trace(const char* fmt, ...);
    void Info (const char* fmt, ...);
    void Warn (const char* fmt, ...);
    void Error(const char* fmt, ...);
    void Clear();
    std::vector<LogEntry> Snapshot(size_t maxEntries = 0); // 0 = all
    // Optional sink, e.g. to mirror into a file or OutputDebugString.
    void SetSink(std::function<void(const LogEntry&)> sink);
    void SetMaxEntries(size_t n);  // Default 5000
}

namespace debug {

using Args = std::vector<std::string>;   // Args[0] is the command name
using CommandFn = std::function<void(const Args&)>;

void RegisterCommand(const char* name, const char* help, CommandFn fn);
void UnregisterCommand(const char* name);
bool Execute(const std::string& line);     // Returns false if unknown command
struct CommandInfo { std::string name, help; };
std::vector<CommandInfo> ListCommands();

void Watch(const char* label, std::function<std::string()> getter, const char* group = "General");
void Unwatch(const char* label);

std::string Fmt(const char* fmt, ...);     // printf -> std::string helper

// Internal
void Init();
void Shutdown();
struct WatchInfo { std::string label, group; std::function<std::string()> getter; };
const std::vector<WatchInfo>& Watches();
const std::vector<std::string>& History();

} // namespace debug
} // namespace blaze
