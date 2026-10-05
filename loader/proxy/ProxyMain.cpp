// dbghelp.dll stub. The game statically imports dbghelp.dll, so Windows loads this from the game folder before the exe
// starts. Every export jumps to System32's dbghelp (Exports.asm, RealDbgHelp.cpp). In the game it also loads dk2ml.dll
// from its own folder.
//
// The two DLLs share only the file name, so either can be updated alone.
#include "Loader.h"

namespace {

// Nothing else writes dk2ml.log when dk2ml.dll doesn't load. kernel32 only: runs under the loader lock.
void LogMissing(const wchar_t* dir, DWORD error)
{
    wchar_t path[MAX_PATH];
    if (swprintf_s(path, L"%sdk2ml.log", dir) < 0) {
        return;
    }

    HANDLE file =
        CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    char line[160];
    int n = sprintf_s(line,
                      "dk2ml.dll did not load from the game folder (error %lu): plugins are off. Copy it next "
                      "to dbghelp.dll.\r\n",
                      error);
    DWORD written = 0;
    if (n > 0) {
        WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr);
    }
    CloseHandle(file);
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    DisableThreadLibraryCalls(instance);

    // Needed in any process. It depends only on core system DLLs, so loading it from DllMain is safe.
    if (!RealDbghelp_Load()) {
        OutputDebugStringA("[dk2ml] cannot load System32\\dbghelp.dll, dbghelp calls will fail\n");
    }

    // only the game gets the loader
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    if (!name || _wcsicmp(name + 1, L"DoorKickers2.exe") != 0) {
        return TRUE;
    }

    // Full path from this file's folder, so the search order can't find another copy. dk2ml.dll imports only system
    // DLLs, so loading it under the loader lock is safe.
    wchar_t dir[MAX_PATH];
    DWORD n = GetModuleFileNameW(instance, dir, MAX_PATH);
    wchar_t* slash = n && n < MAX_PATH ? wcsrchr(dir, L'\\') : nullptr;
    if (!slash) {
        return TRUE;
    }
    slash[1] = L'\0';

    wchar_t loader[MAX_PATH];
    if (swprintf_s(loader, L"%sdk2ml.dll", dir) < 0) {
        return TRUE;
    }
    if (!LoadLibraryExW(loader, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
        DWORD error = GetLastError();
        OutputDebugStringA("[dk2ml] cannot load dk2ml.dll next to dbghelp.dll, mod loader inactive\n");
        LogMissing(dir, error);
    }
    return TRUE;
}
