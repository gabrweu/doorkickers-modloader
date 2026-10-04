// dbghelp.dll: the stub that gets the loader into the game. The game statically imports dbghelp.dll by name, so Windows
// loads this file from the game folder before the exe starts. Every dbghelp export jumps to System32's dbghelp.dll
// (Exports.asm, RealDbgHelp.cpp). In the game, the stub also loads dk2ml.dll from its own folder, and dk2ml.dll's
// DllMain does the rest.
//
// Nothing passes between the two DLLs but the file name, so either can be updated without the other.
#include "Loader.h"

namespace {

// Players look in dk2ml.log, and nothing else writes it when dk2ml.dll doesn't load. kernel32 only, because this runs
// under the loader lock.
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
                      "dk2ml.dll did not load from the game folder (error %lu): native mods are off. Copy it next "
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

    // The real dbghelp, needed in any process that loads this stub. It depends only on core system DLLs, so loading it
    // from DllMain is safe.
    if (!RealDbghelp_Load()) {
        OutputDebugStringA("[dk2ml] cannot load System32\\dbghelp.dll, dbghelp calls will fail\n");
    }

    // Only the game gets the loader, not some other process that happens to load this dbghelp.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* name = wcsrchr(exe, L'\\');
    if (!name || _wcsicmp(name + 1, L"DoorKickers2.exe") != 0) {
        return TRUE;
    }

    // By full path from this file's folder, so the search order can't resolve it elsewhere. dk2ml.dll imports only
    // system DLLs, so loading it under the loader lock is safe too.
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
