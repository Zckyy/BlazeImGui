// =============================================================================
// blaze/config.h - Variable registry + JSON profile saving.
//
// Bind a variable once (e.g. at startup) and BlazeImGui will save/load it with
// configuration profiles. Keys are dotted, lowercase, stable identifiers:
//
//     static bool  g_godMode = false;
//     static float g_timeScale = 1.0f;
//     blaze::config::Bind("player.god_mode", &g_godMode);
//     blaze::config::Bind("world.time_scale", &g_timeScale);
//
// Scopes:
//   Scope::Profile - stored in <configDir>/profiles/<name>.json (default).
//                    Switchable at runtime from the "Configs" page.
//   Scope::Global  - stored in <configDir>/blaze_settings.json, shared by all
//                    profiles (menu theme, blur, hotkeys...).
//
// The bound pointer must outlive the binding (use statics/globals/members and
// call Unbind() in destructors). Defaults are captured at Bind() time and
// restored by ResetToDefaults().
//
// File format (human & AI editable):
//   { "blaze_config_version": 1, "profile": "default",
//     "values": { "player.god_mode": true, "world.time_scale": 1.0 } }
// =============================================================================
#pragma once
#include <string>
#include <vector>
#include <functional>
#include <imgui.h>

namespace blaze::config {

enum class Scope { Profile, Global };

enum class VarType { Bool, Int, Float, String, Color, Key };

void Bind(const char* key, bool*        ptr, Scope scope = Scope::Profile);
void Bind(const char* key, int*         ptr, Scope scope = Scope::Profile);
void Bind(const char* key, float*       ptr, Scope scope = Scope::Profile);
void Bind(const char* key, std::string* ptr, Scope scope = Scope::Profile);
void Bind(const char* key, ImVec4*      ptr, Scope scope = Scope::Profile); // RGBA color
void BindKey(const char* key, int*      vkPtr, Scope scope = Scope::Profile); // Win32 virtual-key code
void Unbind(const char* key);

// Profiles ----------------------------------------------------------------------
std::vector<std::string> ListProfiles();          // Sorted names (no extension)
const std::string&       ActiveProfile();
bool SaveProfile(const std::string& name = {});   // Empty = active profile
bool LoadProfile(const std::string& name);        // Makes it active on success
bool DeleteProfile(const std::string& name);
bool RenameProfile(const std::string& from, const std::string& to);
bool DuplicateProfile(const std::string& from, const std::string& to);
void ResetToDefaults(Scope scope = Scope::Profile);

bool SaveGlobal();
bool LoadGlobal();

// Export / import the active profile as a JSON string (clipboard sharing).
std::string ExportProfileJson();
bool        ImportProfileJson(const std::string& json);

// True when any Profile-scoped value differs from the last save/load.
bool IsDirty();

// Auto-save: when enabled, dirty profiles are written ~1 s after the last change.
void SetAutoSave(bool enabled);
bool GetAutoSave();

const std::string& Directory();   // Absolute config directory
void OpenDirectoryInExplorer();

// Get / set any bound variable (either scope) using JSON text, e.g.
//   SetFromJson("world.time_scale", "2.5");  SetFromJson("world.grid_color", "[1,0,0,1]");
// Returns false if the key is unknown or the JSON type doesn't match the variable.
bool        SetFromJson(const std::string& key, const std::string& jsonValue);
std::string GetJson(const std::string& key);   // Empty string if unknown

// Inspection (used by the Configs page and handy for tools/agents).
struct VarInfo { std::string key; VarType type; Scope scope; std::string valueJson; };
std::vector<VarInfo> ListVars();

// Called when a profile is loaded or reset - re-apply side effects here.
using LoadCallback = std::function<void(const std::string& profile)>;
int  OnProfileLoaded(LoadCallback cb);

// Internal ------------------------------------------------------------------------
void Init(const std::string& directory);
void Tick(float dt);   // Dirty tracking + auto-save. Called by blaze::NewFrame().
void Shutdown();

} // namespace blaze::config
