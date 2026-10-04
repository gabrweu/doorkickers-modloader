#include "Loader.h"

#include <cstdio>

static FILE* g_log = nullptr;
static SRWLOCK g_logLock = SRWLOCK_INIT;
static bool g_echo = false;

// With g_logLock held.
static void WriteLine(const char* line)
{
    if (g_log) {
        fputs(line, g_log);
        fflush(g_log);
    }
    if (g_echo) {
        fputs(line, stdout);
    }
}

void LogEchoToStdout(bool on)
{
    g_echo = on;
}

// Keeps the previous session's log as <name>.prev.log: players often restart after a crash before reporting it.
void LogOpen(const std::wstring& path)
{
    std::wstring previous = path;
    size_t dot = previous.find_last_of(L'.');
    size_t slash = previous.find_last_of(L"\\/");
    bool dotInFolderName = slash != std::wstring::npos && dot < slash;
    if (dot == std::wstring::npos || dotInFolderName) {
        dot = previous.size();
    }
    previous.insert(dot, L".prev");

    MoveFileExW(path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_log = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
}

void LogLine(const char* prefix, const char* fmt, va_list args)
{
    char msg[2048];
    vsnprintf(msg, sizeof(msg), fmt, args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[2200];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] [%s] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
             prefix, msg);

    AcquireSRWLockExclusive(&g_logLock);
    WriteLine(line);
    ReleaseSRWLockExclusive(&g_logLock);
    OutputDebugStringA(line);
}

bool LogTryF(const char* fmt, ...)
{
    // crash report: never waits; the crashing thread may hold the lock
    char msg[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1100];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] [dk2ml] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
             msg);

    if (!TryAcquireSRWLockExclusive(&g_logLock)) {
        return false;
    }
    WriteLine(line);
    ReleaseSRWLockExclusive(&g_logLock);
    return true;
}

std::wstring Log_ModuleName(HMODULE module)
{
    wchar_t path[MAX_PATH];
    if (!module || !GetModuleFileNameW(module, path, MAX_PATH)) {
        return L"plugin";
    }
    std::wstring s = path;
    return s.substr(s.find_last_of(L'\\') + 1);
}

void LogF(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    LogLine("dk2ml", fmt, args);
    va_end(args);
}
