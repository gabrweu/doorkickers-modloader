// Starting point for a Door Kickers 2 plugin, with the everyday pieces:
//   - loader events: every frame, game state changes, map loads (no hooks; any number of mods can listen);
//   - game functions, fields and enum values by name (dk2ml.hpp), resolved once in DK2ML_PluginInit;
//   - a safe hook of your own, with typed arguments;
//   - a hotkey that fires only while the game has focus;
//   - settings on the Modloader screen, saved in the loader's settings folder.
// Replace the behavior with yours. Check names with: tools\symtest.exe "<game folder>" --find "GameClient::*"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

#include "dk2ml.hpp"

// Manifest: shown on the Modloader screen, in dk2ml.log and in crash reports; read without running plugin code.
// First number: the plugin API version needed (DK2ML_API_VERSION of the dk2ml.h built with). Keep the version in step
// with your releases, and gameVersion with mod.xml.
DK2ML_PLUGIN_MANIFEST(1, "My Plugin", "1.0.0", "You", "", 112);

namespace {

const DK2ML_API* g_api = nullptr;

// --- the game, by name (symtest --find / --type / --enum show what exists) ---
dk2ml::Fn<void(void* gameClient, int dt)> UpdateCamera{"GameClient::UpdateCamera"}; // every frame of a mission
dk2ml::Field<uint8_t> GameClient_m_camera{"GameClient", "m_camera"}; // the view Camera, embedded; only Offset() is used
dk2ml::Field<float> Camera_m_fov{"Camera", "m_fov"}; // degrees
dk2ml::Enum StateRunning{"GameClient::eCGameState", "CGAMESTATE_RUNNING"}; // the map is loaded and live

// --- settings: shown on the Modloader screen, saved to <settings folder>\settings.ini ---
bool g_enabled = true;
int g_hotkey = VK_F8;
float g_strength = 1.0f;

std::wstring g_settingsFile; // empty: no settings folder, defaults only

void SaveSettings(const DK2ML_Option*, void*)
{
    if (g_settingsFile.empty()) {
        return;
    }

    auto write = [](const wchar_t* key, const std::wstring& value) {
        WritePrivateProfileStringW(L"settings", key, value.c_str(), g_settingsFile.c_str());
    };

    write(L"enabled", g_enabled ? L"1" : L"0");
    write(L"hotkey", std::to_wstring(g_hotkey));
    write(L"strength", std::to_wstring(g_strength));
}

void LoadSettings()
{
    if (!g_api->GetConfigDir()[0]) {
        return; // no settings folder: defaults only
    }

    g_settingsFile = std::wstring(g_api->GetConfigDir()) + L"settings.ini"; // not the mod folder, which Steam replaces

    g_enabled = GetPrivateProfileIntW(L"settings", L"enabled", g_enabled, g_settingsFile.c_str()) != 0;
    g_hotkey = GetPrivateProfileIntW(L"settings", L"hotkey", g_hotkey, g_settingsFile.c_str());

    // the profile API has no float reader
    wchar_t buf[32] = {};
    GetPrivateProfileStringW(L"settings", L"strength", L"", buf, 32, g_settingsFile.c_str());
    if (buf[0]) {
        g_strength = static_cast<float>(_wtof(buf));
    }
}

void AddOptions()
{
    DK2ML_Option header = {sizeof(DK2ML_Option), DK2ML_OPTION_HEADER, "My plugin"};
    g_api->AddOption(&header);

    DK2ML_Option enabled = {sizeof(DK2ML_Option), DK2ML_OPTION_BOOL, "Enabled", "Turns the mod's effect on or off",
                            &g_enabled};
    enabled.onChange = SaveSettings;
    g_api->AddOption(&enabled);

    DK2ML_Option hotkey = {sizeof(DK2ML_Option), DK2ML_OPTION_KEY, "Toggle key", "Toggles Enabled in a mission",
                           &g_hotkey};
    hotkey.onChange = SaveSettings;
    g_api->AddOption(&hotkey);

    DK2ML_Option strength = {
        sizeof(DK2ML_Option), DK2ML_OPTION_FLOAT, "Strength", nullptr, &g_strength, 0.0f, 2.0f, "%.1fx"};
    strength.onChange = SaveSettings;
    g_api->AddOption(&strength);
}

// --- behavior ---
int g_cameraUpdates = 0; // in this mission
bool g_hotkeyWasDown = false;

// Your own hook, for what no loader event covers. Runs before GameClient::UpdateCamera, with the caller's registers.
int UpdateCameraPre(DK2ML_Regs* regs, void*)
{
    void* client = dk2ml::Arg<void*>(regs, 0); // `this`
    int dt = dk2ml::Arg<int>(regs, 1);
    (void)client;
    (void)dt;

    ++g_cameraUpdates;
    return DK2ML_CALL_ORIGINAL; // or DK2ML_SKIP_ORIGINAL to replace the function (set the result first)
}

void OnFrame(const DK2ML_Event*, void*)
{
    // GetAsyncKeyState also sees keys pressed in other windows
    bool down = g_api->IsGameFocused() && (GetAsyncKeyState(g_hotkey) & 0x8000) != 0;

    if (down && !g_hotkeyWasDown) {
        g_enabled = !g_enabled;
        SaveSettings(nullptr, nullptr);
        g_api->Log("toggled: enabled=%d", g_enabled);
    }

    g_hotkeyWasDown = down;
}

void OnStateChanged(const DK2ML_Event* e, void*)
{
    if (e->newState == StateRunning.Get()) {
        g_cameraUpdates = 0;
        g_api->Log("mission running (enabled=%d, strength=%.1f)", g_enabled, g_strength);
    } else if (e->oldState == StateRunning.Get() && e->gameClient) {
        void* camera = static_cast<char*>(e->gameClient) + GameClient_m_camera.Offset();
        g_api->Log("mission over: %d camera updates, FOV %.0f", g_cameraUpdates, Camera_m_fov(camera));
    }
}

void OnMapLoaded(const DK2ML_Event*, void*)
{
    g_api->Log("map loaded (a mission starts or restarts)");
}

} // namespace

DK2ML_EXPORT int DK2ML_PluginInit(const DK2ML_API* api, const DK2ML_PluginInfo* info)
{
    g_api = api;
    LoadSettings();

    if (!dk2ml::ResolveAll(api)) { // logs every name this game build doesn't have
        return 1;
    }

    // && stops at the first failure
    bool ok = dk2ml::On(api, DK2ML_EVENT_FRAME, OnFrame) && dk2ml::On(api, DK2ML_EVENT_STATE_CHANGED, OnStateChanged) &&
              dk2ml::On(api, DK2ML_EVENT_MAP_LOADED, OnMapLoaded) && dk2ml::Hook(api, UpdateCamera, UpdateCameraPre);
    if (!ok) {
        return 2;
    }

    AddOptions();

    api->Log("ready (settings: %ls)", g_settingsFile.empty() ? L"none" : g_settingsFile.c_str());
    (void)info;
    return 0;
}
