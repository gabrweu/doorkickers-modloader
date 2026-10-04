// The real dbghelp.dll, loaded from System32 by full path (a bare "dbghelp.dll" would resolve to the stub).
// The stub's jumps in Exports.asm go through g_dbghelpReal, which is filled here at process attach; dk2ml.dll and
// symtest only use RealDbghelp().
#include "Loader.h"

namespace {

const char* const kExportNames[] = {
#include "ExportNames.inc"
};
constexpr size_t kExportCount = sizeof(kExportNames) / sizeof(kExportNames[0]);

HMODULE g_real = nullptr;

// Stands in for an export this Windows version's dbghelp doesn't have (the list is generated on the build machine).
// All dbghelp functions report failure with a zero/FALSE result.
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
