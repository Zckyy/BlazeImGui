#include "internal.h"
#include "json.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cmath>

namespace blaze::config {
namespace {

constexpr int kFileVersion = 1;

struct Var {
    std::string key;
    VarType     type;
    Scope       scope;
    void*       ptr;
    json::Value def;
};

std::vector<Var>  g_vars;
std::string       g_dir;
std::string       g_active = "default";
json::Value       g_profileValues = json::Value::MakeObject();  // last loaded/saved (incl. unknown keys)
json::Value       g_globalValues  = json::Value::MakeObject();
std::string       g_profileSnapshot, g_globalSnapshot;          // serialized bound values at last save/load
std::string       g_prevProfile, g_prevGlobal;                  // previous tick, for change debounce
float             g_profileIdle = 0, g_globalIdle = 0;
bool              g_autoSave = true;
bool              g_dirty = false;
bool              g_newProfileKeys = false;   // bound keys missing from the file -> write them once
bool              g_newGlobalKeys = false;
std::vector<std::pair<int, LoadCallback>> g_callbacks;
int               g_nextCallbackId = 1;

json::Value Read(const Var& v) {
    switch (v.type) {
    case VarType::Bool:   return json::Value(*static_cast<bool*>(v.ptr));
    case VarType::Int:
    case VarType::Key:    return json::Value(*static_cast<int*>(v.ptr));
    case VarType::Float: {
        // Round-trip through 6 significant digits so files show 1.4, not 1.39999998.
        char buf[32];
        snprintf(buf, sizeof buf, "%.6g", double(*static_cast<float*>(v.ptr)));
        return json::Value(strtod(buf, nullptr));
    }
    case VarType::String: return json::Value(*static_cast<std::string*>(v.ptr));
    case VarType::Color: {
        auto* c = static_cast<ImVec4*>(v.ptr);
        json::Value a = json::Value::MakeArray();
        for (float f : { c->x, c->y, c->z, c->w }) a.push(json::Value(std::round(double(f) * 1000.0) / 1000.0));
        return a;
    }
    }
    return {};
}

bool Write(const Var& v, const json::Value& j) {
    switch (v.type) {
    case VarType::Bool:
        if (j.isBool()) { *static_cast<bool*>(v.ptr) = j.asBool(); return true; }
        if (j.isNumber()) { *static_cast<bool*>(v.ptr) = j.asNumber() != 0; return true; }
        return false;
    case VarType::Int:
    case VarType::Key:
        if (!j.isNumber()) return false;
        *static_cast<int*>(v.ptr) = int(std::lround(j.asNumber()));
        return true;
    case VarType::Float:
        if (!j.isNumber()) return false;
        *static_cast<float*>(v.ptr) = float(j.asNumber());
        return true;
    case VarType::String:
        if (!j.isString()) return false;
        *static_cast<std::string*>(v.ptr) = j.asString();
        return true;
    case VarType::Color: {
        if (!j.isArray() || j.items().size() < 3) return false;
        auto& it = j.items();
        auto* c = static_cast<ImVec4*>(v.ptr);
        *c = ImVec4(float(it[0].asNumber()), float(it[1].asNumber()), float(it[2].asNumber()),
                    it.size() > 3 ? float(it[3].asNumber(1.0)) : 1.0f);
        return true;
    }
    }
    return false;
}

std::string Snapshot(Scope scope) {
    std::string s;
    for (auto& v : g_vars) {
        if (v.scope != scope) continue;
        s += v.key; s += '='; s += Read(v).dump(0); s += ';';
    }
    return s;
}

std::wstring ProfilesDir() { return json::Widen(g_dir) + L"\\profiles"; }
std::wstring ProfilePath(const std::string& name) { return ProfilesDir() + L"\\" + json::Widen(name) + L".json"; }
std::wstring GlobalPath() { return json::Widen(g_dir) + L"\\blaze_settings.json"; }

std::string Sanitize(const std::string& name) {
    std::string out;
    for (char c : name) {
        unsigned char u = (unsigned char)c;
        if (isalnum(u) || c == ' ' || c == '-' || c == '_' || c == '.' || u >= 0x80) out += c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.substr(0, 64);
}

void ApplyValues(const json::Value& values, Scope scope) {
    for (auto& v : g_vars) {
        if (v.scope != scope) continue;
        if (const json::Value* j = values.find(v.key)) {
            if (!Write(v, *j)) Log::Write(LogLevel::Warn, "config", "Type mismatch for '%s' - kept current value", v.key.c_str());
        }
    }
}

bool LoadFile(const std::wstring& path, json::Value& values, std::string* profileName) {
    std::string text;
    if (!json::ReadFile(path, text)) return false;
    json::Value root; std::string err;
    if (!json::Parse(text, root, &err)) {
        Log::Write(LogLevel::Error, "config", "Failed to parse %s: %s", json::Narrow(path).c_str(), err.c_str());
        return false;
    }
    const json::Value* vals = root.find("values");
    values = (vals && vals->isObject()) ? *vals : json::Value::MakeObject();
    if (profileName) if (auto* p = root.find("profile")) if (p->isString()) *profileName = p->asString();
    return true;
}

bool SaveFile(const std::wstring& path, json::Value& values, Scope scope, const std::string& profileName) {
    // Merge bound values over previously loaded ones so keys from not-yet-bound
    // modules (or hand-edited extras) survive a save.
    for (auto& v : g_vars)
        if (v.scope == scope) values[v.key] = Read(v);

    json::Value root = json::Value::MakeObject();
    root["blaze_config_version"] = json::Value(kFileVersion);
    if (scope == Scope::Profile) root["profile"] = json::Value(profileName);
    else root["kind"] = json::Value("global");
    root["values"] = values;

    SHCreateDirectoryExW(nullptr, scope == Scope::Profile ? ProfilesDir().c_str() : json::Widen(g_dir).c_str(), nullptr);
    if (!json::WriteFileAtomic(path, root.dump(2) + "\n")) {
        Log::Write(LogLevel::Error, "config", "Could not write %s", json::Narrow(path).c_str());
        return false;
    }
    return true;
}

void FireLoaded() {
    for (auto& [id, cb] : g_callbacks) cb(g_active);
}

void BindImpl(const char* key, VarType type, void* ptr, Scope scope) {
    Unbind(key);
    Var v{ key, type, scope, ptr, {} };
    v.def = Read(v);
    // Late binding: apply value already present in the loaded file.
    const json::Value& src = scope == Scope::Profile ? g_profileValues : g_globalValues;
    if (const json::Value* j = src.find(v.key)) Write(v, *j);
    else (scope == Scope::Profile ? g_newProfileKeys : g_newGlobalKeys) = true;
    g_vars.push_back(std::move(v));
    if (scope == Scope::Profile) g_profileSnapshot = g_prevProfile = Snapshot(Scope::Profile);
    else g_globalSnapshot = g_prevGlobal = Snapshot(Scope::Global);
}

} // namespace

void Bind(const char* k, bool* p, Scope s)        { BindImpl(k, VarType::Bool, p, s); }
void Bind(const char* k, int* p, Scope s)         { BindImpl(k, VarType::Int, p, s); }
void Bind(const char* k, float* p, Scope s)       { BindImpl(k, VarType::Float, p, s); }
void Bind(const char* k, std::string* p, Scope s) { BindImpl(k, VarType::String, p, s); }
void Bind(const char* k, ImVec4* p, Scope s)      { BindImpl(k, VarType::Color, p, s); }
void BindKey(const char* k, int* p, Scope s)      { BindImpl(k, VarType::Key, p, s); }

void Unbind(const char* key) {
    g_vars.erase(std::remove_if(g_vars.begin(), g_vars.end(), [&](auto& v) { return v.key == key; }), g_vars.end());
}

std::vector<std::string> ListProfiles() {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((ProfilesDir() + L"\\*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            out.push_back(json::Narrow(n.substr(0, n.size() - 5)));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

const std::string& ActiveProfile() { return g_active; }

bool SaveProfile(const std::string& nameIn) {
    std::string name = Sanitize(nameIn.empty() ? g_active : nameIn);
    if (name.empty()) return false;
    if (name != g_active) {
        // Saving under a new name: start from current values only (plus unknown keys).
        g_active = name;
    }
    if (!SaveFile(ProfilePath(name), g_profileValues, Scope::Profile, name)) return false;
    g_profileSnapshot = g_prevProfile = Snapshot(Scope::Profile);
    g_dirty = false;
    Log::Write(LogLevel::Info, "config", "Saved profile '%s'", name.c_str());
    return true;
}

bool LoadProfile(const std::string& nameIn) {
    std::string name = Sanitize(nameIn);
    json::Value values;
    if (name.empty() || !LoadFile(ProfilePath(name), values, nullptr)) {
        Log::Write(LogLevel::Warn, "config", "Profile '%s' not found", nameIn.c_str());
        return false;
    }
    // Reset to defaults first so keys missing from the file don't keep stale values.
    for (auto& v : g_vars) if (v.scope == Scope::Profile) Write(v, v.def);
    g_profileValues = values;
    ApplyValues(values, Scope::Profile);
    g_active = name;
    g_profileSnapshot = g_prevProfile = Snapshot(Scope::Profile);
    g_dirty = false;
    Log::Write(LogLevel::Info, "config", "Loaded profile '%s'", name.c_str());
    FireLoaded();
    return true;
}

bool DeleteProfile(const std::string& name) {
    if (!DeleteFileW(ProfilePath(Sanitize(name)).c_str())) return false;
    Log::Write(LogLevel::Info, "config", "Deleted profile '%s'", name.c_str());
    if (name == g_active) {
        auto all = ListProfiles();
        if (!all.empty()) LoadProfile(all.front());
        else { g_active = "default"; SaveProfile(); }
    }
    return true;
}

bool RenameProfile(const std::string& from, const std::string& toIn) {
    std::string to = Sanitize(toIn);
    if (to.empty()) return false;
    if (!MoveFileExW(ProfilePath(Sanitize(from)).c_str(), ProfilePath(to).c_str(), 0)) return false;
    if (from == g_active) { g_active = to; SaveProfile(); }
    return true;
}

bool DuplicateProfile(const std::string& from, const std::string& toIn) {
    std::string to = Sanitize(toIn);
    if (to.empty()) return false;
    json::Value values;
    if (!LoadFile(ProfilePath(Sanitize(from)), values, nullptr)) return false;
    json::Value root = json::Value::MakeObject();
    root["blaze_config_version"] = json::Value(kFileVersion);
    root["profile"] = json::Value(to);
    root["values"] = values;
    return json::WriteFileAtomic(ProfilePath(to), root.dump(2) + "\n");
}

void ResetToDefaults(Scope scope) {
    for (auto& v : g_vars) if (v.scope == scope) Write(v, v.def);
    if (scope == Scope::Profile) FireLoaded();
}

bool SaveGlobal() {
    if (!SaveFile(GlobalPath(), g_globalValues, Scope::Global, {})) return false;
    g_globalSnapshot = g_prevGlobal = Snapshot(Scope::Global);
    return true;
}

bool LoadGlobal() {
    json::Value values;
    if (!LoadFile(GlobalPath(), values, nullptr)) return false;
    g_globalValues = values;
    ApplyValues(values, Scope::Global);
    g_globalSnapshot = g_prevGlobal = Snapshot(Scope::Global);
    return true;
}

std::string ExportProfileJson() {
    json::Value values = g_profileValues;
    for (auto& v : g_vars) if (v.scope == Scope::Profile) values[v.key] = Read(v);
    json::Value root = json::Value::MakeObject();
    root["blaze_config_version"] = json::Value(kFileVersion);
    root["profile"] = json::Value(g_active);
    root["values"] = values;
    return root.dump(2);
}

bool ImportProfileJson(const std::string& text) {
    json::Value root; std::string err;
    if (!json::Parse(text, root, &err)) {
        Log::Write(LogLevel::Error, "config", "Import failed: %s", err.c_str());
        return false;
    }
    const json::Value* vals = root.find("values");
    if (!vals || !vals->isObject()) {
        Log::Write(LogLevel::Error, "config", "Import failed: missing \"values\" object");
        return false;
    }
    ApplyValues(*vals, Scope::Profile);
    for (auto& kv : vals->members()) g_profileValues[kv.first] = kv.second;
    Log::Write(LogLevel::Info, "config", "Imported %d values into '%s'", int(vals->members().size()), g_active.c_str());
    FireLoaded();
    return true;
}

bool IsDirty() { return g_dirty; }
void SetAutoSave(bool e) { g_autoSave = e; }
bool GetAutoSave() { return g_autoSave; }
const std::string& Directory() { return g_dir; }

void OpenDirectoryInExplorer() {
    SHCreateDirectoryExW(nullptr, ProfilesDir().c_str(), nullptr);
    ShellExecuteW(nullptr, L"open", json::Widen(g_dir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool SetFromJson(const std::string& key, const std::string& jsonValue) {
    json::Value v;
    if (!json::Parse(jsonValue, v)) return false;
    for (auto& var : g_vars)
        if (var.key == key) return Write(var, v);
    return false;
}

std::string GetJson(const std::string& key) {
    for (auto& var : g_vars)
        if (var.key == key) return Read(var).dump(0);
    return {};
}

std::vector<VarInfo> ListVars() {
    std::vector<VarInfo> out;
    for (auto& v : g_vars) out.push_back({ v.key, v.type, v.scope, Read(v).dump(0) });
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.key < b.key; });
    return out;
}

int OnProfileLoaded(LoadCallback cb) {
    g_callbacks.emplace_back(g_nextCallbackId, std::move(cb));
    return g_nextCallbackId++;
}

void Init(const std::string& directory) {
    g_dir = directory;
    SHCreateDirectoryExW(nullptr, ProfilesDir().c_str(), nullptr);
}

void Tick(float dt) {
    // Profile: dirty flag + debounced auto-save (1 s after the last change).
    std::string cur = Snapshot(Scope::Profile);
    g_dirty = cur != g_profileSnapshot;
    if (cur != g_prevProfile) { g_prevProfile = cur; g_profileIdle = 0; }
    else g_profileIdle += dt;
    if ((g_dirty || g_newProfileKeys) && g_autoSave && g_profileIdle > 1.0f) { g_newProfileKeys = false; SaveProfile(); }

    // Global settings always persist (0.5 s debounce).
    std::string gcur = Snapshot(Scope::Global);
    if (gcur != g_prevGlobal) { g_prevGlobal = gcur; g_globalIdle = 0; }
    else g_globalIdle += dt;
    if ((gcur != g_globalSnapshot || g_newGlobalKeys) && g_globalIdle > 0.5f) { g_newGlobalKeys = false; SaveGlobal(); }
}

void Shutdown() {
    if (g_autoSave && (g_dirty || g_newProfileKeys)) SaveProfile();
    if (Snapshot(Scope::Global) != g_globalSnapshot || g_newGlobalKeys) SaveGlobal();
    g_newProfileKeys = g_newGlobalKeys = false;
    g_vars.clear();
    g_callbacks.clear();
    g_profileValues = json::Value::MakeObject();
    g_globalValues = json::Value::MakeObject();
}

} // namespace blaze::config
