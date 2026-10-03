#include "internal.h"

#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <deque>
#include <mutex>
#include <sstream>

namespace blaze {
namespace {

std::mutex                              g_logMutex;
std::deque<LogEntry>                    g_log;
size_t                                  g_maxEntries = 5000;
std::function<void(const LogEntry&)>    g_sink;

struct Command { std::string name, help; debug::CommandFn fn; };
std::vector<Command>             g_commands;
std::vector<debug::WatchInfo>    g_watches;
std::vector<std::string>         g_history;

std::string VFormat(const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (n <= 0) return {};
    std::string s(size_t(n), '\0');
    vsnprintf(s.data(), size_t(n) + 1, fmt, args);
    return s;
}

void Push(LogLevel level, const char* category, std::string msg) {
    LogEntry e{ level, Time(), category && *category ? category : "app", std::move(msg) };
    std::function<void(const LogEntry&)> sink;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_log.push_back(e);
        while (g_log.size() > g_maxEntries) g_log.pop_front();
        sink = g_sink;
    }
    if (sink) sink(e);
#ifdef _DEBUG
    static const char* tags[] = { "TRACE", "INFO", "WARN", "ERROR" };
    std::string line = "[blaze][" + std::string(tags[int(level)]) + "][" + e.category + "] " + e.message + "\n";
    OutputDebugStringA(line.c_str());
#endif
}

std::vector<std::string> Tokenize(const std::string& line) {
    // Whitespace separated, "double quotes" group words.
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false, any = false;
    for (char c : line) {
        if (c == '"') { quoted = !quoted; any = true; continue; }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (any) { out.push_back(cur); cur.clear(); any = false; }
            continue;
        }
        cur += c; any = true;
    }
    if (any) out.push_back(cur);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------
// Log
// ---------------------------------------------------------------------------------
namespace Log {

#define BLAZE_LOG_IMPL(level)              \
    va_list args; va_start(args, fmt);      \
    std::string s = VFormat(fmt, args);     \
    va_end(args);                           \
    Push(level, nullptr, std::move(s));

void Write(LogLevel level, const char* category, const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    std::string s = VFormat(fmt, args);
    va_end(args);
    Push(level, category, std::move(s));
}
void Trace(const char* fmt, ...) { BLAZE_LOG_IMPL(LogLevel::Trace) }
void Info (const char* fmt, ...) { BLAZE_LOG_IMPL(LogLevel::Info) }
void Warn (const char* fmt, ...) { BLAZE_LOG_IMPL(LogLevel::Warn) }
void Error(const char* fmt, ...) { BLAZE_LOG_IMPL(LogLevel::Error) }
#undef BLAZE_LOG_IMPL

void Clear() { std::lock_guard<std::mutex> lock(g_logMutex); g_log.clear(); }

std::vector<LogEntry> Snapshot(size_t maxEntries) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    size_t start = (maxEntries && g_log.size() > maxEntries) ? g_log.size() - maxEntries : 0;
    return std::vector<LogEntry>(g_log.begin() + std::ptrdiff_t(start), g_log.end());
}

void SetSink(std::function<void(const LogEntry&)> sink) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_sink = std::move(sink);
}

void SetMaxEntries(size_t n) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_maxEntries = std::max<size_t>(n, 16);
}

} // namespace Log

// ---------------------------------------------------------------------------------
// Commands / watches
// ---------------------------------------------------------------------------------
namespace debug {

std::string Fmt(const char* fmt, ...) {
    va_list args; va_start(args, fmt);
    std::string s = VFormat(fmt, args);
    va_end(args);
    return s;
}

void RegisterCommand(const char* name, const char* help, CommandFn fn) {
    UnregisterCommand(name);
    g_commands.push_back({ name, help ? help : "", std::move(fn) });
    std::sort(g_commands.begin(), g_commands.end(), [](auto& a, auto& b) { return a.name < b.name; });
}

void UnregisterCommand(const char* name) {
    g_commands.erase(std::remove_if(g_commands.begin(), g_commands.end(),
        [&](auto& c) { return c.name == name; }), g_commands.end());
}

bool Execute(const std::string& line) {
    Args args = Tokenize(line);
    if (args.empty()) return false;
    if (g_history.empty() || g_history.back() != line) g_history.push_back(line);
    if (g_history.size() > 64) g_history.erase(g_history.begin());

    Log::Write(LogLevel::Trace, "console", "> %s", line.c_str());
    for (auto& c : g_commands) {
        if (_stricmp(c.name.c_str(), args[0].c_str()) == 0) {
            c.fn(args);
            return true;
        }
    }
    Log::Write(LogLevel::Warn, "console", "Unknown command '%s'. Type 'help' for a list.", args[0].c_str());
    return false;
}

std::vector<CommandInfo> ListCommands() {
    std::vector<CommandInfo> out;
    for (auto& c : g_commands) out.push_back({ c.name, c.help });
    return out;
}

void Watch(const char* label, std::function<std::string()> getter, const char* group) {
    Unwatch(label);
    g_watches.push_back({ label, group ? group : "General", std::move(getter) });
}

void Unwatch(const char* label) {
    g_watches.erase(std::remove_if(g_watches.begin(), g_watches.end(),
        [&](auto& w) { return w.label == label; }), g_watches.end());
}

const std::vector<WatchInfo>&  Watches() { return g_watches; }
const std::vector<std::string>& History() { return g_history; }

void Init() {
    RegisterCommand("help", "List all commands", [](const Args&) {
        for (auto& c : g_commands)
            Log::Write(LogLevel::Info, "console", "%-18s %s", c.name.c_str(), c.help.c_str());
    });
    RegisterCommand("clear", "Clear the log", [](const Args&) { Log::Clear(); });
    RegisterCommand("echo", "echo <text...> - print text", [](const Args& a) {
        std::string s;
        for (size_t i = 1; i < a.size(); ++i) { if (i > 1) s += ' '; s += a[i]; }
        Log::Write(LogLevel::Info, "console", "%s", s.c_str());
    });
}

void Shutdown() {
    g_commands.clear();
    g_watches.clear();
    g_history.clear();
}

} // namespace debug
} // namespace blaze
