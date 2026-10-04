// System32's dbghelp.dll, by full path: a bare "dbghelp.dll" resolves to the stub. Fills g_dbghelpReal for the stub's
// jumps (Exports.asm); dk2ml.dll and symtest use only RealDbghelp().
#include "Loader.h"

namespace {

const char* const kExportNames[] = {
#include "ExportNames.inc"
};
constexpr size_t kExportCount = sizeof(kExportNames) / sizeof(kExportNames[0]);

HMODULE g_real = nullptr;

// For exports this Windows' dbghelp lacks (the list comes from the build machine). Zero/FALSE is dbghelp's failure.
extern "C" uintptr_t MissingExport()
{
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}

} // namespace

extern "C" void* g_dbghelpReal[kExportCount] = {};

bool RealDbghelp_Load()
{
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 16) {
        return false;
    }
    wcscat_s(path, L"\\dbghelp.dll");

    g_real = LoadLibraryW(path);
    for (size_t i = 0; i < kExportCount; ++i) {
        FARPROC p = g_real ? GetProcAddress(g_real, kExportNames[i]) : nullptr;
        g_dbghelpReal[i] = p ? reinterpret_cast<void*>(p) : reinterpret_cast<void*>(&MissingExport);
    }
    return g_real != nullptr;
}

HMODULE RealDbghelp()
{
    return g_real;
}
