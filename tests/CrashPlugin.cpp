// Crashes the game on purpose to test the crash report (dk2ml-crash-*.log) in game. Never ship it.
// Put build\crash_plugin.dll in <game>\mods_native\, start the game, then:
// - Ctrl+Shift+F12: a plugin thread writes to address 0. Plugin threads aren't contained, so the game's crash handler
//   runs; the loader's report is written first.
// - Ctrl+Shift+F11: the same write in its FRAME callback, which is contained: the plugin is switched off and the game
//   goes on. A plugin thread then tries CreateSafeHook, which must be refused (dk2ml.log); its Modloader page says
//   it crashed and was switched off for this session.
// Remove the DLL afterwards.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "dk2ml.h"

DK2ML_PLUGIN_MANIFEST(1, "Crash test (don't ship)", "1.0.0", "dk2ml", "", 112);

namespace {

const DK2ML_API* g_api = nullptr;
void* g_target = nullptr; // hooked again after the contained crash
bool g_fired = false;

DWORD WINAPI CrashThread(void*)
{
    *static_cast<volatile int*>(nullptr) = 1;
    return 0;
}

int Pre(DK2ML_Regs*, void*)
{
    return DK2ML_CALL_ORIGINAL;
}

DWORD WINAPI HookAgainThread(void*)
{
    Sleep(2000);

    DK2ML_Status s = g_target ? g_api->CreateSafeHook(g_target, Pre, nullptr, nullptr) : DK2ML_ERROR;
    g_api->Log("after the contained crash: CreateSafeHook returned %d (expected refused, %d)", s, DK2ML_ERROR);
    return 0;
}

bool Down(int key)
{
    return (GetAsyncKeyState(key) & 0x8000) != 0;
}

void OnFrame(const DK2ML_Event*, void*)
{
    bool armed = !g_fired && g_api->IsGameFocused() && Down(VK_CONTROL) && Down(VK_SHIFT);
    if (!armed) {
        return;
    }

    if (Down(VK_F12)) {
        g_fired = true;
        g_api->Log("Ctrl+Shift+F12: crashing the game on purpose (crash report test)");
        CloseHandle(CreateThread(nullptr, 0, CrashThread, nullptr, 0, nullptr));
    } else if (Down(VK_F11)) {
        g_fired = true;
        g_api->Log("Ctrl+Shift+F11: crashing in the FRAME callback on purpose (contained crash test)");
        CloseHandle(CreateThread(nullptr, 0, HookAgainThread, nullptr, 0, nullptr));
        *static_cast<volatile int*>(nullptr) = 1;
    }
}

} // namespace

DK2ML_EXPORT int DK2ML_PluginInit(const DK2ML_API* api, const DK2ML_PluginInfo*)
{
    g_api = api;
    if (api->Subscribe(DK2ML_EVENT_FRAME, OnFrame, nullptr) != DK2ML_OK) {
        return 1;
    }

    g_target = api->ResolveSymbol("GameGUI::IsAnyMenuOpened");
    api->Log("loaded: Ctrl+Shift+F12 crashes the game, Ctrl+Shift+F11 crashes contained (crash tests)");
    return 0;
}
