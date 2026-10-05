#include "Loader.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace {

CrashHelpers g_helpers = {};
wchar_t g_dir[MAX_PATH] = {}; // where reports go, with trailing backslash
wchar_t g_lastPath[MAX_PATH] = {};
DWORD g_mainThread = 0;
volatile LONG g_written = 0;

constexpr size_t kTextSize = 64 * 1024;
char g_text[kTextSize];

constexpr int kMaxFrames = 64;
constexpr int kMaxPostFrames = 64; // SafeHook.cpp's side-stack depth (kMaxDepth)
constexpr int kMaxHookOwners = 8; // owners named per hooked function
constexpr int kMaxStackMods = 8; // distinct mods listed under "Mods' code on the stack"
// ownerOf's text. Every receiving buffer has this size: strcpy_s into a smaller one ends the process.
constexpr size_t kOwnerText = 512;
constexpr int kKeepReports = 10;

const char* CodeName(DWORD code)
{
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error (memory couldn't be read from disk)";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer division by zero";
    case EXCEPTION_INT_OVERFLOW: return "integer overflow";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "misaligned data";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float division by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "invalid float operation";
    case EXCEPTION_FLT_OVERFLOW: return "float overflow";
    case EXCEPTION_FLT_UNDERFLOW: return "float underflow";
    case EXCEPTION_FLT_INEXACT_RESULT: return "inexact float result";
    case EXCEPTION_FLT_DENORMAL_OPERAND: return "denormal float operand";
    case EXCEPTION_FLT_STACK_CHECK: return "float stack check";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "non-continuable exception";
    case EXCEPTION_INVALID_HANDLE: return "invalid handle";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case 0xE06D7363: return "C++ exception (throw nobody caught)";
    case 0xC0000409: return "stack buffer overrun / fail fast";
    case 0xC0000374: return "heap corruption";
    }
    return "exception";
}

// An access violation's first parameter.
const char* AccessKind(ULONG_PTR kind)
{
    switch (kind) {
    case 0: return "reading";
    case 1: return "writing";
    case 8: return "executing";
    }
    return "using";
}

void Narrow(const wchar_t* w, char* out, size_t size)
{
    if (!size) {
        return;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, static_cast<int>(size), nullptr, nullptr);
    if (n <= 0) {
        out[0] = 0;
    }
    out[size - 1] = 0;
}

// A module's file name, or "module@<base>" if it has none that fits.
void ModuleName(HMODULE module, char* name, size_t size)
{
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        _snprintf_s(name, size, _TRUNCATE, "module@%p", static_cast<void*>(module));
        return;
    }
    const wchar_t* file = wcsrchr(path, L'\\');
    Narrow(file ? file + 1 : path, name, size);
}

bool ModuleOf(uintptr_t address, uintptr_t* base, char* name, size_t size)
{
    void* b = nullptr;
    if (!RtlPcToFileHeader(reinterpret_cast<void*>(address), &b) || !b) {
        return false;
    }
    *base = reinterpret_cast<uintptr_t>(b);
    ModuleName(static_cast<HMODULE>(b), name, size);
    return true;
}

bool InGameExe(uintptr_t address)
{
    uintptr_t base = 0;
    char module[128];
    if (!ModuleOf(address, &base, module, sizeof(module))) {
        return false;
    }
    return base == reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

DWORD ExeTimeStamp()
{
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(GetModuleHandleW(nullptr));
    if (!dos) {
        return 0;
    }
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(dos) + dos->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
}

// "DoorKickers2.exe!Function+0x12", "x.dll+0x1234", or the bare address
void Where(uintptr_t address, const CrashHelpers& helpers, char* out, size_t size)
{
    char module[128];
    uintptr_t base = 0;
    if (!ModuleOf(address, &base, module, sizeof(module))) {
        _snprintf_s(out, size, _TRUNCATE, "0x%llX (in no module: generated code or a bad address)",
                    static_cast<unsigned long long>(address));
        return;
    }

    char symbol[512];
    if (helpers.describe && helpers.describe(address, symbol, sizeof(symbol))) {
        _snprintf_s(out, size, _TRUNCATE, "%s!%s", module, symbol);
    } else {
        _snprintf_s(out, size, _TRUNCATE, "%s+0x%llX", module, static_cast<unsigned long long>(address - base));
    }
}

// "a.dll, b.dll" for the safe hooks on a function, "" if none (or the hook list is busy)
void HookOwners(uintptr_t target, char* out, size_t size)
{
    out[0] = 0;
    HMODULE owners[kMaxHookOwners];
    int n = target ? SafeHook_OwnersOfTry(reinterpret_cast<void*>(target), owners, kMaxHookOwners) : 0;

    size_t len = 0;
    for (int i = 0; i < n && len + 1 < size; ++i) {
        char name[128];
        ModuleName(owners[i], name, sizeof(name));
        int w = _snprintf_s(out + len, size - len, _TRUNCATE, "%s%s", i ? ", " : "", name);
        len = w < 0 ? size - 1 : len + w;
    }
}

// No destructors in here (__try).
// Every read is checked against this thread's stack limits; anything odd ends the walk.
int WalkGuarded(CONTEXT* ctx, CrashFrame* frames, int max, const SafeHookPostFrame* posts, int postCount)
{
    uintptr_t postEntry = SafeHook_PostEntryAddress();
    ULONG_PTR low = 0, high = 0;
    GetCurrentThreadStackLimits(&low, &high);

    int n = 0;
    int post = 0;
    void* reachedThroughPost = nullptr;
    __try {
        while (n < max && ctx->Rip) {
            DWORD64 pc = ctx->Rip;
            DWORD64 sp = ctx->Rsp;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
            frames[n].pc = pc;
            frames[n].functionStart = function ? imageBase + function->BeginAddress : 0;
            frames[n].returnedThroughPost = reachedThroughPost;
            ++n;
            reachedThroughPost = nullptr;

            if (function) {
                void* handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, function, ctx, &handlerData, &establisher, nullptr);
            } else { // a leaf, or code without unwind data: the return address is on top
                if (sp < low || sp + 8 > high) {
                    break;
                }
                ctx->Rip = *reinterpret_cast<const DWORD64*>(sp);
                ctx->Rsp = sp + 8;
            }

            // returned into SafeHookPostEntry: the real address is on the side stack, innermost first
            if (ctx->Rip == postEntry) {
                if (post >= postCount) {
                    break;
                }
                reachedThroughPost = posts[post].target;
                ctx->Rip = posts[post].returnAddress;
                ++post;
            }

            bool movedUp = ctx->Rsp > sp && ctx->Rsp >= low && ctx->Rsp < high;
            if (!movedUp) {
                break; // a frame must move up the stack
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

void AddStackMods(const CrashFrame* frames, int count, const CrashHelpers& helpers, CrashText* out)
{
    char owner[kOwnerText];
    static char seen[kMaxStackMods][kOwnerText]; // static: little stack is left after a stack overflow; runs once
    int seenCount = 0;
    for (int i = 0; i < count && seenCount < kMaxStackMods; ++i) {
        if (!helpers.ownerOf || !helpers.ownerOf(frames[i].pc, owner, sizeof(owner))) {
            continue;
        }
        bool known = false;
        for (int k = 0; k < seenCount; ++k) {
            known |= strcmp(seen[k], owner) == 0;
        }
        if (!known) {
            strcpy_s(seen[seenCount++], owner);
        }
    }

    out->Add("Mods' code on the stack: ");
    for (int k = 0; k < seenCount; ++k) {
        out->Add("%s%s", k ? "; " : "", seen[k]);
    }
    out->Add(seenCount ? "\r\n" : "none\r\n");
}

void AddStack(const CrashFrame* frames, int count, const CrashHelpers& helpers, CrashText* out)
{
    out->Add("\r\nStack (innermost first):\r\n");
    for (int i = 0; i < count; ++i) {
        char where[640];
        Where(frames[i].pc, helpers, where, sizeof(where));
        out->Add("  #%-2d 0x%016llX  %s", i, static_cast<unsigned long long>(frames[i].pc), where);

        char hooks[256];
        HookOwners(frames[i].functionStart, hooks, sizeof(hooks));
        if (hooks[0]) {
            out->Add("  [hooked by %s]", hooks);
        }

        if (frames[i].returnedThroughPost) {
            char target[640];
            Where(reinterpret_cast<uintptr_t>(frames[i].returnedThroughPost), helpers, target, sizeof(target));
            HookOwners(reinterpret_cast<uintptr_t>(frames[i].returnedThroughPost), hooks, sizeof(hooks));
            out->Add("  [called %s, which has a post hook by %s]", target, hooks[0] ? hooks : "?");
        }

        char owner[kOwnerText];
        if (helpers.ownerOf && helpers.ownerOf(frames[i].pc, owner, sizeof(owner))) {
            out->Add("  <- %s", owner);
        }
        out->Add("\r\n");
    }
    if (!count) {
        out->Add("  (couldn't be walked)\r\n");
    }
}

void AddRegisters(const CONTEXT& c, CrashText* out)
{
    out->Add("\r\nRegisters:\r\n");
    out->Add("  rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX\r\n", c.Rax, c.Rbx, c.Rcx, c.Rdx);
    out->Add("  rsi=%016llX rdi=%016llX rbp=%016llX rsp=%016llX\r\n", c.Rsi, c.Rdi, c.Rbp, c.Rsp);
    out->Add("  r8 =%016llX r9 =%016llX r10=%016llX r11=%016llX\r\n", c.R8, c.R9, c.R10, c.R11);
    out->Add("  r12=%016llX r13=%016llX r14=%016llX r15=%016llX\r\n", c.R12, c.R13, c.R14, c.R15);
    out->Add("  rip=%016llX eflags=%08lX\r\n", c.Rip, c.EFlags);
}

void WriteReport(const EXCEPTION_POINTERS* p)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf_s(g_lastPath, L"%sdk2ml-crash-%04u%02u%02u-%02u%02u%02u.log", g_dir, t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond);

    CrashText text{g_text, kTextSize, 0};
    g_text[0] = 0;
    Crash_Format(*p->ExceptionRecord, *p->ContextRecord, GetCurrentThreadId() == g_mainThread, g_helpers, &text);

    HANDLE file =
        CreateFileW(g_lastPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        g_lastPath[0] = 0;
        return;
    }
    DWORD written = 0;
    WriteFile(file, g_text, static_cast<DWORD>(text.len), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);

    LogTryF("the game crashed; crash report: %ls", g_lastPath);
}

bool WriteGuarded(const EXCEPTION_POINTERS* p)
{
    __try {
        WriteReport(p);
        return g_lastPath[0] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

void CrashText::Add(const char* fmt, ...)
{
    if (len + 1 >= cap) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    int n = _vsnprintf_s(buf + len, cap - len, _TRUNCATE, fmt, args);
    va_end(args);
    len = n < 0 ? cap - 1 : len + n; // cut off: the buffer ends with the NUL
}

int Crash_Walk(const CONTEXT& context, CrashFrame* frames, int max)
{
    SafeHookPostFrame posts[kMaxPostFrames];
    int postCount = SafeHook_PostFrames(posts, kMaxPostFrames);
    CONTEXT copy = context;
    return WalkGuarded(&copy, frames, max, posts, postCount);
}

void Crash_Format(const EXCEPTION_RECORD& record, const CONTEXT& context, bool mainThread, const CrashHelpers& helpers,
                  CrashText* out)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    DWORD exeStamp = ExeTimeStamp();
    out->Add("Door Kickers 2 crashed. Report by the Modloader %s (plugin API v%d).\r\n", DK2ML_VERSION,
             DK2ML_API_VERSION);
    out->Add("%04u-%02u-%02u %02u:%02u:%02u, game exe build 0x%08lX, thread %lu (%s)\r\n\r\n", t.wYear, t.wMonth,
             t.wDay, t.wHour, t.wMinute, t.wSecond, exeStamp, GetCurrentThreadId(),
             mainThread ? "the game's main thread" : "not the game's main thread");

    auto address = reinterpret_cast<uintptr_t>(record.ExceptionAddress);
    out->Add("What: %s (0x%08lX)", CodeName(record.ExceptionCode), record.ExceptionCode);
    bool memoryFault =
        record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR;
    if (memoryFault && record.NumberParameters >= 2) {
        out->Add(", %s address 0x%llX", AccessKind(record.ExceptionInformation[0]),
                 static_cast<unsigned long long>(record.ExceptionInformation[1]));
    }

    char where[640];
    Where(address, helpers, where, sizeof(where));
    out->Add("\r\nWhere: %s\r\n", where);

    CrashFrame frames[kMaxFrames];
    int count = Crash_Walk(context, frames, kMaxFrames);

    // whose code: the crash site, then every mod with code on the stack (once each)
    char owner[kOwnerText];
    if (helpers.ownerOf && helpers.ownerOf(address, owner, sizeof(owner))) {
        out->Add("In a mod's code: %s\r\n", owner);
    } else if (InGameExe(address)) {
        out->Add("In the game's own code\r\n");
    } else {
        uintptr_t base = 0;
        char module[128];
        bool inModule = ModuleOf(address, &base, module, sizeof(module));
        out->Add("In %s (not a mod's code)\r\n", inModule ? module : "no module");
    }
    AddStackMods(frames, count, helpers, out);

    AddStack(frames, count, helpers, out);
    AddRegisters(context, out);

    if (helpers.mods) {
        helpers.mods(out);
    }
    out->Add("\r\nThe game's own crash dump and message come after this report. When you report the crash to a mod's "
             "author, send this file and dk2ml.log.\r\n");
}

void CrashReport_SetHelpers(const CrashHelpers& helpers)
{
    g_helpers = helpers;
}

void CrashReport_Init(const std::wstring& dir)
{
    g_mainThread = GetCurrentThreadId();
    wcsncpy_s(g_dir, dir.c_str(), _TRUNCATE);

    // keep the newest reports only (names sort by time); this session's may add one
    std::vector<std::wstring> reports;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"dk2ml-crash-*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                reports.push_back(fd.cFileName);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::sort(reports.begin(), reports.end());
    for (size_t i = 0; i + (kKeepReports - 1) < reports.size(); ++i) {
        DeleteFileW((dir + reports[i]).c_str());
    }
}

bool CrashReport_Write(const EXCEPTION_POINTERS* pointers)
{
    if (!pointers || !pointers->ExceptionRecord || !pointers->ContextRecord || !g_dir[0]) {
        return false;
    }
    if (InterlockedExchange(&g_written, 1)) { // once: a second crash while writing, or another thread's
        return false;
    }
    return WriteGuarded(pointers);
}

const wchar_t* CrashReport_LastPath()
{
    return g_lastPath;
}
