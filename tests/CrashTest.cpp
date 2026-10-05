// The crash report (loader/safety/CrashReport.cpp) without the game: the stack walk on this exe's code, through a
// post hook's swapped return address; the text with fake helpers; writing once; keeping the newest files.
// usage: crashtest.exe [--show]   (--show also prints a sample report)
#include "../loader/Loader.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "MinHook.h"

int main(int argc, char** argv); // the walk checks look for main's frame

namespace {

int g_failures = 0;

void Expect(const char* test, bool ok)
{
    printf("%-60s %s\n", test, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

const HMODULE kOwner = reinterpret_cast<HMODULE>(0x10000);
volatile int g_sink = 0;

// the last walk's result
CrashFrame g_frames[64];
int g_count = 0;
CONTEXT g_context;
void (*g_inside)() = nullptr; // runs inside Walk3, while its callers' frames are live

uintptr_t StartOf(uintptr_t pc)
{
    DWORD64 base = 0;
    PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(pc, &base, nullptr);
    return f ? base + f->BeginAddress : 0;
}

int IndexOf(void* function)
{
    for (int i = 0; i < g_count; ++i) {
        if (g_frames[i].functionStart == reinterpret_cast<uintptr_t>(function)) {
            return i;
        }
    }
    return -1;
}

// Each does work after its call, so no call becomes a tail jump and every frame stays.
extern "C" __declspec(noinline) int Walk3(int x)
{
    RtlCaptureContext(&g_context);
    g_count = Crash_Walk(g_context, g_frames, 64);
    if (g_inside) {
        g_inside();
    }
    return x + g_sink;
}

extern "C" __declspec(noinline) int Walk2(int x)
{
    int r = Walk3(x + 1);
    g_sink += r;
    return r * 2;
}

extern "C" __declspec(noinline) int Walk1(int x)
{
    int r = Walk2(x + 1);
    g_sink += r;
    return r * 3;
}

// Post-hooked: while it runs, its return address is SafeHookPostEntry. Work before the call keeps the call out of the
// bytes MinHook moves into its trampoline.
extern "C" __declspec(noinline) int Hooked(int x)
{
    g_sink += x * 7;
    g_sink ^= x;
    int r = Walk3(x);
    g_sink += r;
    return r + 1;
}

extern "C" __declspec(noinline) int CallsHooked(int x)
{
    int r = Hooked(x);
    g_sink += r;
    return r * 5;
}

int PrePass(DK2ML_Regs*, void*)
{
    return DK2ML_CALL_ORIGINAL;
}

void PostPass(DK2ML_Regs*, void*) {}

// --- the report's text, with the loader's knowledge faked ---
bool FakeDescribe(uintptr_t address, char* out, size_t size)
{
    if (StartOf(address) != reinterpret_cast<uintptr_t>(&Walk3)) {
        return false;
    }

    _snprintf_s(out, size, _TRUNCATE, "Walk3+0x%llX", static_cast<unsigned long long>(address - StartOf(address)));
    return true;
}

bool FakeOwner(uintptr_t address, char* out, size_t size)
{
    if (StartOf(address) != reinterpret_cast<uintptr_t>(&Walk3)) {
        return false;
    }

    _snprintf_s(out, size, _TRUNCATE, "fake.dll (mod \"Fake\")");
    return true;
}

void FakeMods(CrashText* out)
{
    out->Add("\r\nPlugins (1):\r\n  \"Fake\": running\r\n");
}

const CrashHelpers kFake = {FakeDescribe, FakeOwner, FakeMods};

EXCEPTION_RECORD FakeAccessViolation()
{
    EXCEPTION_RECORD r = {};
    r.ExceptionCode = EXCEPTION_ACCESS_VIOLATION;
    r.ExceptionAddress = reinterpret_cast<void*>(g_context.Rip);
    r.NumberParameters = 2;
    r.ExceptionInformation[0] = 1; // write
    r.ExceptionInformation[1] = 0x10;
    return r;
}

char g_text[64 * 1024];

void FormatInside() // the report for a fake crash in Walk3, formatted while Walk3..main are on the stack
{
    EXCEPTION_RECORD record = FakeAccessViolation();
    CrashText out{g_text, sizeof(g_text), 0};
    g_text[0] = 0;
    Crash_Format(record, g_context, true, kFake, &out);
}

bool Contains(const char* text, const char* part)
{
    return strstr(text, part) != nullptr;
}

std::string ReadAll(const std::wstring& path)
{
    std::string s;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return s;
    }

    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) {
        s.append(buf, n);
    }
    CloseHandle(h);
    return s;
}

// --- the sections, in main's order ---

// The walks run from main: the checks expect main right after the walked functions.
void CheckPlainWalk()
{
    int i3 = IndexOf(&Walk3);
    int i2 = IndexOf(&Walk2);
    int i1 = IndexOf(&Walk1);
    int im = IndexOf(&main);
    Expect("walk: Walk3, Walk2, Walk1, main in order", i3 == 0 && i2 == 1 && i1 == 2 && im == 3);

    bool noPost = g_count > 3 && !g_frames[1].returnedThroughPost && !g_frames[2].returnedThroughPost;
    Expect("walk: no post hook on the way", noPost);
}

bool HookHooked()
{
    void* target = reinterpret_cast<void*>(&Hooked);
    return SafeHook_Create(target, PrePass, PostPass, nullptr, kOwner) == DK2ML_OK &&
           SafeHook_SetEnabled(target, kOwner, true) > 0;
}

void CheckWalkThroughPost()
{
    int ih = IndexOf(&Hooked);
    int ic = IndexOf(&CallsHooked);
    Expect("walk through a post hook: Hooked, then its real caller", ih == 1 && ic == 2 && IndexOf(&main) == 3);
    Expect("walk through a post hook: the frame says which post hook",
           ic == 2 && g_frames[ic].returnedThroughPost == reinterpret_cast<void*>(&Hooked));

    HMODULE owners[4];
    bool ownerRead = SafeHook_OwnersOfTry(reinterpret_cast<void*>(&Hooked), owners, 4) == 1 && owners[0] == kOwner;
    Expect("the hook's owner can be read without waiting", ownerRead);

    SafeHookPostFrame none[4];
    Expect("after the call nothing waits on the side stack", SafeHook_PostFrames(none, 4) == 0);
}

void CheckReportText(const char* text, bool show)
{
    Expect("report: what, with the access", Contains(text, "access violation (0xC0000005), writing address 0x10"));
    Expect("report: where, named by the helper", Contains(text, "crashtest.exe!Walk3+0x"));
    Expect("report: whose code crashed", Contains(text, "In a mod's code: fake.dll (mod \"Fake\")"));
    Expect("report: mods' code on the stack, once",
           Contains(text, "Mods' code on the stack: fake.dll (mod \"Fake\")\r\n"));
    Expect("report: the stack names the frames",
           Contains(text, "#0 ") && Contains(text, "#3 ") && Contains(text, "<- fake.dll"));
    Expect("report: registers, the mods, the main thread",
           Contains(text, "rip=") && Contains(text, "Plugins (1)") && Contains(text, "the game's main thread"));

    if (show) {
        printf("----\n%s----\n", text);
    }
}

void CheckCutOff(const EXCEPTION_RECORD& record)
{
    char small[300 + 16];
    memset(small, 'Z', sizeof(small));
    CrashText cut{small, 300, 0};
    Crash_Format(record, g_context, false, kFake, &cut);

    bool guardOk = true;
    for (size_t k = 300; k < sizeof(small); ++k) {
        guardOk &= small[k] == 'Z';
    }
    Expect("report: cut off at the buffer's end, NUL-terminated", cut.len == 299 && small[299] == 0 && guardOk);
}

std::wstring MakeReportDir()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring dir = std::wstring(temp) + L"dk2ml_crashtest\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// 12 old reports, so startup has some to delete
void MakeOldReports(const std::wstring& dir)
{
    for (int k = 0; k < 12; ++k) {
        wchar_t name[64];
        swprintf_s(name, L"dk2ml-crash-20000101-0000%02d.log", k);
        HANDLE h = CreateFileW((dir + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        CloseHandle(h);
    }
}

void CheckOldReportsTrimmed(const std::wstring& dir)
{
    int left = 0;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"dk2ml-crash-*.log").c_str(), &fd);
    bool oldestGone = true;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            ++left;
            oldestGone &= wcscmp(fd.cFileName, L"dk2ml-crash-20000101-000002.log") > 0;
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    Expect("startup keeps the 9 newest reports (room for this session's)", left == 9 && oldestGone);
}

void CheckWrite(const std::wstring& dir, EXCEPTION_RECORD& record)
{
    CrashReport_SetHelpers(kFake);
    EXCEPTION_POINTERS pointers = {&record, &g_context};
    bool first = CrashReport_Write(&pointers);
    std::wstring written = CrashReport_LastPath();
    std::string content = ReadAll(written);

    bool inFolder = first && written.rfind(dir, 0) == 0 && Contains(content.c_str(), "access violation");
    Expect("the report is written to the folder", inFolder);
    Expect("only once per process", !CrashReport_Write(&pointers));
    Expect("no report without the exception", !CrashReport_Write(nullptr));
}

void RemoveReportDir(const std::wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((dir + L"*.log").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            DeleteFileW((dir + fd.cFileName).c_str());
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir.c_str());
}

} // namespace

int main(int argc, char** argv)
{
    LogOpen(L"crashtest.log");
    if (MH_Initialize() != MH_OK) {
        printf("MH_Initialize failed\n");
        return 1;
    }

    Walk1(1);
    CheckPlainWalk();

    // Hooked returns to SafeHookPostEntry; the walk follows the side stack to the real caller
    Expect("a post hook on Hooked", HookHooked());
    CallsHooked(3);
    CheckWalkThroughPost();

    g_inside = FormatInside;
    Walk1(1);
    g_inside = nullptr;
    EXCEPTION_RECORD record = FakeAccessViolation();
    CheckReportText(g_text, argc > 1 && strcmp(argv[1], "--show") == 0);

    CheckCutOff(record);

    std::wstring dir = MakeReportDir();
    MakeOldReports(dir);
    CrashReport_Init(dir);
    CheckOldReportsTrimmed(dir);
    CheckWrite(dir, record);

    RemoveReportDir(dir);

    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
