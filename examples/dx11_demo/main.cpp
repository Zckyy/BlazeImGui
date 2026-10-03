// =============================================================================
// BlazeImGui DX11 demo
//
// A stand-in "game": a shader-drawn synthwave scene whose parameters are driven
// by the debug menu, so you can see the acrylic blur, live tweaking, profiling,
// watches, console commands and config profiles working together.
//
// Controls: Insert toggles the menu (rebindable in Settings > Input).
//
// The integration itself is ~15 lines - search for "blaze::" in this file.
// =============================================================================
#include <blaze/blaze.h>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <imgui_impl_win32.h>
#include <shellapi.h>

#include "capture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

// -----------------------------------------------------------------------------
// D3D11 boilerplate
// -----------------------------------------------------------------------------
static ID3D11Device*           g_device = nullptr;
static ID3D11DeviceContext*    g_context = nullptr;
static IDXGISwapChain1*        g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT                    g_resizeW = 0, g_resizeH = 0;
static UINT                    g_width = 1600, g_height = 900;

static ID3D11VertexShader* g_sceneVS = nullptr;
static ID3D11PixelShader*  g_scenePS = nullptr;
static ID3D11Buffer*       g_sceneCB = nullptr;

static void CreateRTV() {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
    D3D11_TEXTURE2D_DESC d; back->GetDesc(&d);
    g_width = d.Width; g_height = d.Height;
    back->Release();
}

static bool CreateDevice(HWND hwnd) {
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2,
                                   D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) // debug layer not installed
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                               D3D11_SDK_VERSION, &g_device, nullptr, &g_context);
    if (FAILED(hr)) return false;

    IDXGIDevice* dxgiDevice = nullptr; IDXGIAdapter* adapter = nullptr; IDXGIFactory2* factory = nullptr;
    g_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(IID_PPV_ARGS(&factory));

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    hr = factory->CreateSwapChainForHwnd(g_device, hwnd, &sd, nullptr, nullptr, &g_swapChain);
    if (FAILED(hr)) { sd.Flags = 0; hr = factory->CreateSwapChainForHwnd(g_device, hwnd, &sd, nullptr, nullptr, &g_swapChain); }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release(); adapter->Release(); dxgiDevice->Release();
    if (FAILED(hr)) return false;
    CreateRTV();
    return true;
}

// -----------------------------------------------------------------------------
// The "game": a fullscreen shader scene
// -----------------------------------------------------------------------------
static const char* kSceneHLSL = R"HLSL(
cbuffer Scene : register(b0) {
    float2 res; float time; float timeOfDay;
    float4 gridColor;
    float4 sunColor;
    float  orbs; float speed; float2 playerPos;
};
float4 VSMain(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
float hash(float2 p) { return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453); }
float4 PSMain(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy / res;
    float2 p = (pos.xy - 0.5 * res) / res.y; p.y = -p.y;
    const float horizon = -0.05;
    float day = sin(saturate(timeOfDay) * 3.14159);

    float3 skyTop = lerp(float3(0.02, 0.01, 0.07), float3(0.16, 0.38, 0.86), day);
    float3 skyBot = lerp(float3(0.38, 0.05, 0.36), float3(0.98, 0.60, 0.38), day);
    float3 col = lerp(skyBot, skyTop, saturate((p.y - horizon) * 1.6));

    // Stars
    float2 sg = floor(pos.xy / 3.0);
    float star = step(0.9975, hash(sg)) * (1.0 - day) * step(horizon, p.y);
    col += star * (0.6 + 0.4 * sin(time * 3.0 + hash(sg) * 50.0));

    // Striped sun
    float2 sp = p - float2(0.0, 0.13);
    float d = length(sp);
    float sun = smoothstep(0.262, 0.255, d);
    float stripes = step(0.45 + sp.y * 2.5, frac(sp.y * 22.0 - time * 0.4 * speed));
    sun *= sp.y > -0.01 ? 1.0 : stripes;
    float3 sunGrad = lerp(float3(1.0, 0.85, 0.35), float3(1.0, 0.25, 0.6), saturate(-sp.y * 3.0 + 0.4));
    col = lerp(col, sunGrad * sunColor.rgb, sun * step(horizon, p.y));
    col += sunColor.rgb * 0.22 * exp(-d * 3.5) * step(horizon, p.y);

    // Mountains silhouette
    float m = horizon + 0.05 + 0.06 * sin(p.x * 6.0 + 1.3) * sin(p.x * 2.3) + 0.02 * sin(p.x * 23.0);
    if (p.y < m && p.y > horizon) col = lerp(col, float3(0.05, 0.02, 0.09) + gridColor.rgb * 0.08, 0.92);

    // Neon grid floor
    if (p.y < horizon) {
        float z = 0.25 / (horizon - p.y);
        float x = (p.x - playerPos.x * 0.05) * z;
        float gz = z + time * speed * 2.0 + playerPos.y;
        float2 g = abs(frac(float2(x, gz)) - 0.5);
        float ln = min(g.x / (fwidth(x) * 1.5), g.y / (fwidth(gz) * 1.5));
        float gridL = 1.0 - saturate(ln);
        float fade = saturate(1.0 - z * 0.035);
        col = lerp(float3(0.03, 0.01, 0.06), float3(0.08, 0.04, 0.12), day);
        col += gridColor.rgb * gridL * fade * 1.5;
        col += gridColor.rgb * 0.25 * exp(-(horizon - p.y) * 10.0);
    }

    // Floating orbs
    for (int i = 0; i < 16; ++i) {
        if (i >= (int)orbs) break;
        float fi = (float)i;
        float2 c = float2(sin(time * 0.5 * speed + fi * 1.7) * 0.75, 0.12 + cos(time * 0.7 * speed + fi * 2.3) * 0.28);
        float r = 0.025 + 0.012 * sin(fi * 3.1);
        float dd = length(p - c);
        float3 oc = 0.55 + 0.45 * cos(6.2831 * (fi * 0.13 + float3(0.0, 0.33, 0.67)));
        col += oc * smoothstep(r, r * 0.4, dd);
        col += oc * 0.004 / (dd * dd + 0.002);
    }

    float2 v = uv - 0.5;
    col *= 1.0 - dot(v, v) * 0.9;
    return float4(saturate(col), 1.0);
}
)HLSL";

struct alignas(16) SceneCB {
    float res[2]; float time; float timeOfDay;
    float gridColor[4];
    float sunColor[4];
    float orbs; float speed; float playerPos[2];
};

static bool CreateScene() {
    ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(kSceneHLSL, strlen(kSceneHLSL), "scene", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0, &vs, &err)) ||
        FAILED(D3DCompile(kSceneHLSL, strlen(kSceneHLSL), "scene", nullptr, nullptr, "PSMain", "ps_5_0", 0, 0, &ps, &err))) {
        if (err) { OutputDebugStringA((const char*)err->GetBufferPointer()); err->Release(); }
        return false;
    }
    g_device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_sceneVS);
    g_device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_scenePS);
    vs->Release(); ps->Release();
    D3D11_BUFFER_DESC bd{ sizeof(SceneCB), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    return SUCCEEDED(g_device->CreateBuffer(&bd, nullptr, &g_sceneCB));
}

// -----------------------------------------------------------------------------
// Game state exposed to the debug menu
// -----------------------------------------------------------------------------
struct Entity { int id; std::string name; std::string type; float x, y; int health; bool alive; };

static struct Game {
    // Player
    bool   godMode = false;
    bool   infiniteAmmo = false;
    bool   noclip = false;
    float  moveSpeed = 6.0f;
    float  jumpHeight = 1.4f;
    int    noclipKey = 'N';
    float  pos[2] = { 0, 0 };
    // World
    float  timeOfDay = 0.18f;
    float  timeScale = 1.0f;
    bool   pauseTime = false;
    int    orbCount = 8;
    int    weather = 0;
    ImVec4 gridColor = ImVec4(1.0f, 0.18f, 0.75f, 1.0f);
    ImVec4 sunColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    // Engine
    bool   vsync = true;
    float  simulatedLoadMs = 1.5f;
    // Runtime
    double time = 0;
    std::vector<Entity> entities;
    int    nextId = 1;
    int    selected = -1;
} game;

static void SpawnEntities(int n) {
    static const char* types[] = { "Grunt", "Drone", "Sentinel", "Courier", "Turret" };
    for (int i = 0; i < n; ++i) {
        Entity e;
        e.id = game.nextId++;
        e.type = types[e.id % 5];
        e.name = e.type + std::string("_") + std::to_string(e.id);
        e.x = float((e.id * 37) % 200) - 100.0f;
        e.y = float((e.id * 91) % 200) - 100.0f;
        e.health = 40 + (e.id * 13) % 60;
        e.alive = true;
        game.entities.push_back(e);
    }
}

// Busy-wait to simulate CPU work so the profiler has something to show.
static void BurnCpu(double ms) {
    LARGE_INTEGER f, s, n; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&s);
    do { QueryPerformanceCounter(&n); } while (double(n.QuadPart - s.QuadPart) * 1000.0 / double(f.QuadPart) < ms);
}

static void UpdateGame(float dt) {
    BLAZE_PROFILE_SCOPE("Game update");
    if (!game.pauseTime) {
        game.time += dt * game.timeScale;
        game.timeOfDay = fmodf(game.timeOfDay + dt * game.timeScale * 0.004f, 1.0f);
    }
    {
        BLAZE_PROFILE_SCOPE("AI");
        BurnCpu(game.simulatedLoadMs * 0.6);
    }
    {
        BLAZE_PROFILE_SCOPE("Physics");
        BurnCpu(game.simulatedLoadMs * 0.4);
        game.pos[0] = sinf(float(game.time) * 0.3f) * 10.0f;
        game.pos[1] = float(game.time) * game.moveSpeed * 0.05f;
    }
    int alive = 0;
    for (auto& e : game.entities) alive += e.alive;
    blaze::perf::SetCounter("Entities", alive);
    blaze::perf::SetCounter("Draw calls", 1 + game.orbCount * 2);
}

static void RenderScene() {
    BLAZE_PROFILE_SCOPE("Scene render");
    SceneCB cb{};
    cb.res[0] = float(g_width); cb.res[1] = float(g_height);
    cb.time = float(game.time);
    cb.timeOfDay = game.timeOfDay;
    memcpy(cb.gridColor, &game.gridColor, sizeof(float) * 4);
    memcpy(cb.sunColor, &game.sunColor, sizeof(float) * 4);
    cb.orbs = float(game.orbCount);
    cb.speed = 1.0f;
    cb.playerPos[0] = game.pos[0]; cb.playerPos[1] = game.pos[1];
    g_context->UpdateSubresource(g_sceneCB, 0, nullptr, &cb, 0, 0);

    D3D11_VIEWPORT vp{ 0, 0, float(g_width), float(g_height), 0, 1 };
    g_context->RSSetViewports(1, &vp);
    g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_context->IASetInputLayout(nullptr);
    g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_context->VSSetShader(g_sceneVS, nullptr, 0);
    g_context->PSSetShader(g_scenePS, nullptr, 0);
    g_context->PSSetConstantBuffers(0, 1, &g_sceneCB);
    g_context->Draw(3, 0);
}

// -----------------------------------------------------------------------------
// Custom debug pages - this is what you'd write for your own game
// -----------------------------------------------------------------------------
static void RegisterGamePanels() {
    using namespace blaze;

    // Config bindings: saved per profile, restored on load.
    config::Bind("player.god_mode", &game.godMode);
    config::Bind("player.infinite_ammo", &game.infiniteAmmo);
    config::Bind("player.noclip", &game.noclip);
    config::Bind("player.move_speed", &game.moveSpeed);
    config::Bind("player.jump_height", &game.jumpHeight);
    config::BindKey("player.noclip_key", &game.noclipKey);
    config::Bind("world.time_scale", &game.timeScale);
    config::Bind("world.orb_count", &game.orbCount);
    config::Bind("world.weather", &game.weather);
    config::Bind("world.grid_color", &game.gridColor);
    config::Bind("world.sun_color", &game.sunColor);
    config::Bind("engine.vsync", &game.vsync);
    config::Bind("engine.simulated_load_ms", &game.simulatedLoadMs);

    Panel player;
    player.id = "game.player";
    player.title = "Player";
    player.subtitle = "Cheats and movement tuning";
    player.category = "Game";
    player.icon = icons::Person;
    player.order = 0;
    player.draw = [] {
        using namespace blaze::ui;
        if (BeginCard("Cheats", icons::Flash)) {
            if (Toggle("God mode", &game.godMode, "Player takes no damage"))
                Log::Info("God mode %s", game.godMode ? "enabled" : "disabled");
            Toggle("Infinite ammo", &game.infiniteAmmo);
            Toggle("No-clip", &game.noclip, "Fly through geometry");
            KeyBind("No-clip hotkey", &game.noclipKey);
        }
        EndCard();
        if (BeginCard("Movement", icons::Game)) {
            SliderFloat("Move speed", &game.moveSpeed, 0.0f, 20.0f, "%.1f m/s", "Also scrolls the grid floor");
            SliderFloat("Jump height", &game.jumpHeight, 0.0f, 5.0f, "%.2f m");
        }
        EndCard();
        if (BeginCard("Transform", icons::Target)) {
            KeyValue("Position X", "%.2f", game.pos[0]);
            KeyValue("Position Z", "%.2f", game.pos[1]);
            ImGui::Dummy(ImVec2(0, 2));
            if (Button("Teleport to origin", ButtonKind::Primary)) { debug::Execute("tp 0 0"); }
        }
        EndCard();
    };
    RegisterPanel(player);

    Panel world;
    world.id = "game.world";
    world.title = "World";
    world.subtitle = "Time, weather and visuals - changes apply to the scene live";
    world.category = "Game";
    world.icon = icons::Globe;
    world.order = 10;
    world.draw = [] {
        using namespace blaze::ui;
        if (BeginCard("Time", icons::Clock)) {
            SliderFloat("Time of day", &game.timeOfDay, 0.0f, 1.0f, "%.2f", "0 = midnight, 0.5 = noon");
            SliderFloat("Time scale", &game.timeScale, 0.0f, 10.0f, "%.1fx");
            Toggle("Pause time", &game.pauseTime);
        }
        EndCard();
        if (BeginCard("Visuals", icons::Color)) {
            SliderInt("Orbs", &game.orbCount, 0, 16, "%d", "Number of floating lights");
            const char* weathers[] = { "Clear", "Overcast", "Rain", "Storm", "Fog" };
            Combo("Weather", &game.weather, weathers, 5);
            ColorEdit("Grid colour", &game.gridColor, "Neon floor lines");
            ColorEdit("Sun tint", &game.sunColor);
        }
        EndCard();
        if (BeginCard("Engine", icons::Processing)) {
            Toggle("V-Sync", &game.vsync, "Turn off to see uncapped frame rate in the Performance page");
            SliderFloat("Simulated CPU load", &game.simulatedLoadMs, 0.0f, 20.0f, "%.1f ms", "Busy-work split into the AI and Physics scopes");
        }
        EndCard();
    };
    RegisterPanel(world);

    Panel ents;
    ents.id = "game.entities";
    ents.title = "Entities";
    ents.subtitle = "Inspect and manipulate spawned entities";
    ents.category = "Game";
    ents.icon = icons::List;
    ents.order = 20;
    ents.draw = [] {
        using namespace blaze::ui;
        const auto& c = theme::Colors();
        static char filter[64] = "";
        if (Button("Spawn 10", ButtonKind::Primary)) debug::Execute("spawn 10");
        ImGui::SameLine();
        if (Button("Kill all", ButtonKind::Danger)) debug::Execute("kill_all");
        ImGui::SameLine();
        if (Button("Clear dead", ButtonKind::Subtle)) {
            game.entities.erase(std::remove_if(game.entities.begin(), game.entities.end(),
                [](const Entity& e) { return !e.alive; }), game.entities.end());
            game.selected = -1;
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##entfilter", "Filter by name or type", filter, sizeof filter);

        if (BeginCard("World entities", icons::List)) {
            if (ImGui::BeginTable("##ents", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                                  ImVec2(0, 280 * GetUIScale()))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 40 * GetUIScale());
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Type");
                ImGui::TableSetupColumn("Health");
                ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 80 * GetUIScale());
                ImGui::TableHeadersRow();
                for (int i = 0; i < int(game.entities.size()); ++i) {
                    Entity& e = game.entities[size_t(i)];
                    if (filter[0] && e.name.find(filter) == std::string::npos && e.type.find(filter) == std::string::npos) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    char id[16]; snprintf(id, sizeof id, "%d", e.id);
                    if (ImGui::Selectable(id, game.selected == i, ImGuiSelectableFlags_SpanAllColumns)) game.selected = i;
                    ImGui::TableNextColumn(); ImGui::TextUnformatted(e.name.c_str());
                    ImGui::TableNextColumn(); ImGui::TextColored(c.textMuted, "%s", e.type.c_str());
                    ImGui::TableNextColumn();
                    ImVec4 hc = e.health > 60 ? c.success : e.health > 25 ? c.warning : c.error;
                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetTextLineHeight() * 0.5f);
                    ProgressBar(e.health / 100.0f, ImVec2(-1, 5 * GetUIScale()), &hc);
                    ImGui::TableNextColumn();
                    Badge(e.alive ? "Alive" : "Dead", e.alive ? c.success : c.textDisabled);
                }
                ImGui::EndTable();
            }
        }
        EndCard();

        if (game.selected >= 0 && game.selected < int(game.entities.size())) {
            Entity& e = game.entities[size_t(game.selected)];
            if (BeginCard(e.name.c_str(), icons::Target, "Selected entity")) {
                KeyValue("Type", "%s", e.type.c_str());
                KeyValue("Position", "%.1f, %.1f", e.x, e.y);
                SliderInt("Health", &e.health, 0, 100);
                Toggle("Alive", &e.alive);
            }
            EndCard();
        }
    };
    RegisterPanel(ents);

    // Watches: live values shown in Debug > Watches
    debug::Watch("Position", [] { return debug::Fmt("%.2f, %.2f", game.pos[0], game.pos[1]); }, "Player");
    debug::Watch("God mode", [] { return std::string(game.godMode ? "on" : "off"); }, "Player");
    debug::Watch("Game time", [] { return debug::Fmt("%.1f s", game.time); }, "World");
    debug::Watch("Time of day", [] { return debug::Fmt("%.2f", game.timeOfDay); }, "World");
    debug::Watch("Entities", [] { return std::to_string(game.entities.size()); }, "World");

    // Console commands
    debug::RegisterCommand("spawn", "spawn <count> - spawn entities", [](const debug::Args& a) {
        int n = a.size() > 1 ? std::max(1, atoi(a[1].c_str())) : 1;
        SpawnEntities(n);
        Log::Info("Spawned %d entities (%d total)", n, int(game.entities.size()));
    });
    debug::RegisterCommand("kill_all", "Kill every entity", [](const debug::Args&) {
        for (auto& e : game.entities) e.alive = false;
        Log::Warn("Killed all %d entities", int(game.entities.size()));
    });
    debug::RegisterCommand("tp", "tp <x> <z> - teleport the player", [](const debug::Args& a) {
        if (a.size() < 3) { Log::Error("usage: tp <x> <z>"); return; }
        game.pos[0] = float(atof(a[1].c_str()));
        game.pos[1] = float(atof(a[2].c_str()));
        Log::Info("Teleported to %.1f, %.1f", game.pos[0], game.pos[1]);
        Notify("Teleported", ToastKind::Success, 2.0f);
    });
    debug::RegisterCommand("timeofday", "timeofday <0-1>", [](const debug::Args& a) {
        if (a.size() > 1) game.timeOfDay = float(atof(a[1].c_str()));
    });
}

// -----------------------------------------------------------------------------
// Win32
// -----------------------------------------------------------------------------
static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (blaze::WndProcHandler(hwnd, msg, wParam, lParam)) return 1;   // <- blaze
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) { g_resizeW = LOWORD(lParam); g_resizeH = HIWORD(lParam); }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Command line (all optional) - handy for automated screenshots / AI agents:
//   --capture <file.png>   Render hidden, save the back buffer after --delay seconds, exit
//   --delay <seconds>      Default 1.5 (lets animations settle)
//   --page <panel id>      Open a page, e.g. blaze.performance, blaze.settings, game.world
//   --exec "<command>"     Run a console command after startup (repeatable)
//   --config-dir <dir>     Config folder (capture mode defaults to %TEMP%\BlazeDemoCapture)
//   --size <W>x<H>         Client size, default 1600x900
//   --drag <x>,<y>,<dx>,<dy>  Inject a left-button drag (client pixels) 0.5 s after start,
//                          spread over 12 frames. Verifies dragging/sliders headlessly.
//   --dump <file.json>     With --capture: also write blaze::DumpStateJson() (includes the
//                          main window rect) when the capture is taken
struct Options {
    std::wstring capturePath;
    float        delay = 1.5f;
    std::string  page;
    std::vector<std::string> exec;
    std::string  configDir;
    bool         drag = false;
    float        dragX = 0, dragY = 0, dragDX = 0, dragDY = 0;
    std::wstring dumpPath;
};

// Synthetic mouse input for --drag: press, move in equal steps, release.
static void InjectDrag(const Options& o) {
    static int step = -1;   // -1 = not started, 0 = pressed, 1..12 = moving, 13 = done
    constexpr int kMoves = 12;
    if (!o.drag || step > kMoves || blaze::Time() < 0.5) return;
    ImGuiIO& io = ImGui::GetIO();
    if (step < 0) {
        io.AddMousePosEvent(o.dragX, o.dragY);
        io.AddMouseButtonEvent(0, true);
    } else if (step < kMoves) {
        float t = float(step + 1) / kMoves;
        io.AddMousePosEvent(o.dragX + o.dragDX * t, o.dragY + o.dragDY * t);
    } else {
        io.AddMouseButtonEvent(0, false);
    }
    ++step;
}

static std::string ToUtf8(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static Options ParseOptions() {
    Options o;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        bool hasNext = i + 1 < argc;
        if (a == L"--capture" && hasNext) o.capturePath = argv[++i];
        else if (a == L"--delay" && hasNext) o.delay = float(_wtof(argv[++i]));
        else if (a == L"--page" && hasNext) o.page = ToUtf8(argv[++i]);
        else if (a == L"--exec" && hasNext) o.exec.push_back(ToUtf8(argv[++i]));
        else if (a == L"--config-dir" && hasNext) o.configDir = ToUtf8(argv[++i]);
        else if (a == L"--size" && hasNext) swscanf_s(argv[++i], L"%ux%u", &g_width, &g_height);
        else if (a == L"--drag" && hasNext)
            o.drag = swscanf_s(argv[++i], L"%f,%f,%f,%f", &o.dragX, &o.dragY, &o.dragDX, &o.dragDY) == 4;
        else if (a == L"--dump" && hasNext) o.dumpPath = argv[++i];
    }
    LocalFree(argv);
    if (!o.capturePath.empty() && o.configDir.empty()) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        o.configDir = ToUtf8(tmp) + "BlazeDemoCapture";
    }
    return o;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    Options opt = ParseOptions();
    const bool capturing = !opt.capturePath.empty();
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{ sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInst, LoadIcon(nullptr, IDI_APPLICATION),
                    LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"BlazeImGuiDemo", nullptr };
    RegisterClassExW(&wc);
    RECT r{ 0, 0, LONG(g_width), LONG(g_height) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"BlazeImGui Demo - press Insert to toggle the menu", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, hInst, nullptr);
    if (!CreateDevice(hwnd) || !CreateScene()) {
        MessageBoxW(hwnd, L"Failed to create Direct3D 11 device", L"BlazeImGui Demo", MB_ICONERROR);
        return 1;
    }
    if (!capturing) {
        ShowWindow(hwnd, SW_SHOWDEFAULT);
        UpdateWindow(hwnd);
    }

    // ---- blaze: initialize -------------------------------------------------
    blaze::InitInfo info;
    info.device = g_device;
    info.context = g_context;
    info.swapChain = g_swapChain;
    info.hwnd = hwnd;
    info.appName = "Blaze Demo";
    if (!opt.configDir.empty()) info.configDir = opt.configDir.c_str();
    if (!blaze::Initialize(info)) return 1;

    SpawnEntities(24);
    RegisterGamePanels();
    if (!opt.page.empty()) blaze::SelectPanel(opt.page);
    if (capturing) blaze::SetMenuOpen(true);
    for (auto& cmd : opt.exec) blaze::debug::Execute(cmd);
    blaze::Log::Info("Demo scene ready. Try the console: 'help', 'spawn 5', 'tp 3 4', 'blaze.theme light'");
    blaze::Log::Warn("This is what a warning looks like");
    blaze::Log::Write(blaze::LogLevel::Error, "net", "Example error from the 'net' category");

    LARGE_INTEGER freq, last, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (g_resizeW && g_resizeH) {
            blaze::OnResizeBegin();                                       // <- blaze
            g_rtv->Release(); g_rtv = nullptr;
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
            CreateRTV();
            blaze::OnResizeEnd();                                         // <- blaze
            g_resizeW = g_resizeH = 0;
        }

        QueryPerformanceCounter(&now);
        float dt = float(double(now.QuadPart - last.QuadPart) / double(freq.QuadPart));
        last = now;

        UpdateGame(dt);
        RenderScene();

        InjectDrag(opt);
        blaze::NewFrame();        // <- blaze: build the menu
        blaze::Render(g_rtv);     // <- blaze: blur + draw on top of the scene

        if (capturing && blaze::Time() >= opt.delay) {
            ID3D11Texture2D* back = nullptr;
            g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
            bool ok = capture::SaveTexturePng(g_device, g_context, back, opt.capturePath);
            back->Release();
            if (!opt.dumpPath.empty()) {
                FILE* f = nullptr;
                std::string state = blaze::DumpStateJson();
                if (_wfopen_s(&f, opt.dumpPath.c_str(), L"wb") == 0 && f) {
                    fwrite(state.data(), 1, state.size(), f);
                    fclose(f);
                } else {
                    ok = false;
                }
            }
            blaze::Shutdown();
            return ok ? 0 : 2;
        }

        HRESULT hr = g_swapChain->Present(game.vsync ? 1 : 0, game.vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING);
        if (hr == DXGI_ERROR_INVALID_CALL) g_swapChain->Present(game.vsync ? 1 : 0, 0);
    }

    blaze::Shutdown();            // <- blaze: saves configs
    if (g_sceneCB) g_sceneCB->Release();
    if (g_scenePS) g_scenePS->Release();
    if (g_sceneVS) g_sceneVS->Release();
    if (g_rtv) g_rtv->Release();
    if (g_swapChain) g_swapChain->Release();
    if (g_context) g_context->Release();
    if (g_device) g_device->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInst);
    return 0;
}
