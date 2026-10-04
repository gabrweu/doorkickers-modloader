// dk2ml.dll, the loader. The game statically imports dbghelp.dll, and the dbghelp.dll stub in the game folder
// (proxy/ProxyMain.cpp) loads this from its DllMain, so it runs before the exe's entry point.
//
// DllMain runs under the loader lock, so it only hooks the exe's entry point. The detour loads the symbols and plugins
// on the main thread before any game code runs, then calls the original entry.
#include "Loader.h"

#include <malloc.h> // _resetstkoflw

#include "MinHook.h"

namespace {

using EntryFn = int(WINAPI*)();
EntryFn g_originalEntry = nullptr;

std::wstring GameDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path);
    return dir.substr(0, dir.find_last_of(L'\\') + 1);
}

// CreateMiniDump is the game's top-level exception filter. WinMain installs it, then patches SetUnhandledExceptionFilter
// so it can't be replaced. It runs only for unhandled exceptions, on the crashing thread, before the game's dump and
// message.
int CrashHandlerPre(DK2ML_Regs* regs, void*)
{
    CrashReport_Write(reinterpret_cast<const EXCEPTION_POINTERS*>(regs->rcx));
    return DK2ML_CALL_ORIGINAL;
}

// target: CreateMiniDump, null if this game build has none. self: the loader's module, the hook's owner.
bool InstallCrashHook(void* target, HMODULE self)
{
    if (!target) {
        return false;
    }
    if (SafeHook_Create(target, CrashHandlerPre, nullptr, nullptr, self) != DK2ML_OK) {
        return false;
    }
    return SafeHook_SetEnabled(target, self, true) > 0;
}

void HookCrashHandler(const std::wstring& gameDir)
{
    CrashReport_Init(gameDir);
    CrashReport_SetHelpers({Symbols_DescribeTry, Plugins_CrashOwner, Plugins_CrashMods});

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&HookCrashHandler), &self);
    void* target = Symbols_Resolve("CreateMiniDump");
    if (InstallCrashHook(target, self)) {
        Symbols_WarnIfShared(target, "loader");
        LogF("crash reports: on (dk2ml-crash-*.log in the game folder)");
    } else {
        LogF("crash reports: off, the game's crash handler (CreateMiniDump) isn't available in this game build");
    }
}

__declspec(noinline) void StartLoader()
{
    std::wstring gameDir = GameDir();
    std::wstring ini = gameDir + L"dk2ml.ini";
    Consent_EnsureIni(ini); // the release is only the two DLLs, so the default dk2ml.ini is written here

    if (GetPrivateProfileIntW(L"loader", L"enabled", 1, ini.c_str()) == 0) {
        LogF("disabled in dk2ml.ini");
    } else if (Symbols_Init(gameDir, GetModuleHandleW(nullptr))) {
        HookCrashHandler(gameDir);
        Plugins_LoadAll(gameDir);
        GameHooks_Init();
        Plugins_LogSharedHooks();
    } else {
        LogF("symbols unavailable, no plugins loaded");
    }
}

// No destructors in here (__try).
bool StartLoaderContained(DWORD* code)
{
    __try {
        StartLoader();
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int WINAPI EntryDetour()
{
    LogOpen(GameDir() + L"dk2ml.log");
    LogF("Door Kickers 2 native mod loader %s, api v%d", DK2ML_VERSION, DK2ML_API_VERSION);

    // If the loader faults while starting (a bug, or a mod file it can't handle), the game starts unmodded, so the
    // fault can't keep the game from starting. Crashes in a plugin's init are contained in Plugins.cpp and don't reach
    // this. A crash in a plugin's DllMain does.
    DWORD code = 0;
    if (!StartLoaderContained(&code)) {
        if (code == EXCEPTION_STACK_OVERFLOW) {
            _resetstkoflw();
        }
        // Every hook made before the failure (plugins, the menu) goes back to the original code. That includes the
        // entry point hook, which is safe because this detour is already running and the trampoline stays valid.
        MH_DisableHook(MH_ALL_HOOKS);
        LogF("the loader crashed while starting (exception 0x%08lX): native mods are off for this session", code);
        std::wstring text = L"The native mod loader ran into an error while starting, so native mods are off for this "
                            L"session. The game itself starts normally.\n\n"
                            L"Details are in dk2ml.log in the game folder (the last plugin it names may be the cause).";
        MessageBoxW(nullptr, text.c_str(), L"Door Kickers 2 - native mods",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
    }
    return g_originalEntry();
}

void* ExeEntryPoint()
{
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return base + nt->OptionalHeader.AddressOfEntryPoint;
}

bool HookEntryPoint(void* entry)
{
    if (MH_Initialize() != MH_OK) {
        return false;
    }
    MH_STATUS created =
        MH_CreateHook(entry, reinterpret_cast<void*>(&EntryDetour), reinterpret_cast<void**>(&g_originalEntry));
    if (created != MH_OK) {
        return false;
    }
    return MH_EnableHook(entry) == MH_OK;
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    DisableThreadLibraryCalls(instance);

    // The real dbghelp for the PDB lookups (Symbols.cpp). The stub has already loaded it, so this only takes a reference.
    if (!RealDbghelp_Load()) {
        OutputDebugStringA("[dk2ml] cannot load System32\\dbghelp.dll, no symbols\n");
    }

    // Only hook when loaded by the game (the stub checks this too).
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    if (!name || _wcsicmp(name + 1, L"DoorKickers2.exe") != 0) {
        return TRUE;
    }

    if (!HookEntryPoint(ExeEntryPoint())) {
        OutputDebugStringA("[dk2ml] failed to hook exe entry point, mod loader inactive\n");
    }
    return TRUE;
}
