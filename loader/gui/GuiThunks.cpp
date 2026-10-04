// GuiSetCallback thunks and CaptureGameInput. No game names or hooks: guitest runs it.
//
// pCallback is void(const GUI::sAction&) with no user slot, so each (plugin, fn, user) gets one thunk,
// `mov rdx, entry; mov rax, Dispatch; jmp rax`; Dispatch calls the plugin under __try. The game calls pCallback
// through a pointer, so rdx is free. Clones share the thunk; the action's owner names the item. Never freed: items
// point at thunks until a GUI reload.
#include "Loader.h"

#include <malloc.h> // _resetstkoflw

#include <algorithm>
#include <cstring>
#include <deque>

namespace {

struct Entry {
    DK2ML_GuiCallbackFn fn;
    void* user;
    HMODULE owner;
    void* thunk;
};

constexpr size_t kThunkSize = 32;
constexpr size_t kMaxThunks = 4096;

SRWLOCK g_lock = SRWLOCK_INIT;     // the lists below; dispatch only reads g_faulted
std::deque<Entry> g_entries;       // stable addresses: thunks point at them
std::vector<HMODULE> g_faulted;    // switched off: their thunks do nothing
std::vector<HMODULE> g_capturing;  // CaptureGameInput(1)

GuiCallbackLayout g_layout = {-1, -1};
void (*g_onCrash)(HMODULE owner) = nullptr;

uint8_t* g_thunks = nullptr; // kMaxThunks * kThunkSize, committed at the first thunk
size_t g_thunksUsed = 0;

thread_local DWORD t_code;

int Filter(DWORD code)
{
    t_code = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

// No destructors in here (__try).
bool Call(const Entry* e, void* item, float x, float y)
{
    __try {
        e->fn(item, x, y, e->user);
        return true;
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

bool IsFaulted(HMODULE owner)
{
    AcquireSRWLockShared(&g_lock);
    bool faulted = std::find(g_faulted.begin(), g_faulted.end(), owner) != g_faulted.end();
    ReleaseSRWLockShared(&g_lock);
    return faulted;
}

// Thunk target: rcx = the running GUI::sAction, rdx = our entry.
void Dispatch(const void* action, const Entry* e)
{
    if (!action || IsFaulted(e->owner)) {
        return;
    }

    const char* a = static_cast<const char*>(action);
    void* item = g_layout.actionOwner >= 0 ? *reinterpret_cast<void* const*>(a + g_layout.actionOwner) : nullptr;
    float x = 0, y = 0;
    if (g_layout.actionCursor >= 0) {
        const float* cursor = reinterpret_cast<const float*>(a + g_layout.actionCursor); // Vector2 {x, y}
        x = cursor[0];
        y = cursor[1];
    }
    if (Call(e, item, x, y)) {
        return;
    }

    if (t_code == EXCEPTION_STACK_OVERFLOW) {
        _resetstkoflw();
    }
    LogF("%ls crashed in its GUI callback (exception 0x%08lX)", Log_ModuleName(e->owner).c_str(), t_code);
    Gui_FaultOwner(e->owner);
    if (g_onCrash) {
        g_onCrash(e->owner); // its hooks, events and options too
    }
}

void* WriteThunk(const Entry* entry) // under g_lock
{
    if (!g_thunks) {
        g_thunks = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, kMaxThunks * kThunkSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (!g_thunks) {
            return nullptr;
        }
    }
    if (g_thunksUsed >= kMaxThunks) {
        return nullptr;
    }

    DWORD old;
    // Stays executable: another thread may be running a thunk on this page.
    if (!VirtualProtect(g_thunks, kMaxThunks * kThunkSize, PAGE_EXECUTE_READWRITE, &old)) {
        return nullptr;
    }

    uint8_t* p = g_thunks + g_thunksUsed * kThunkSize;
    uint8_t* s = p;
    auto put64 = [&](uint64_t v) {
        memcpy(s, &v, 8);
        s += 8;
    };
    // clang-format off
    *s++ = 0x48; *s++ = 0xBA; put64(reinterpret_cast<uint64_t>(entry));    // mov rdx, entry
    *s++ = 0x48; *s++ = 0xB8; put64(reinterpret_cast<uint64_t>(&Dispatch)); // mov rax, Dispatch
    *s++ = 0xFF; *s++ = 0xE0;                                               // jmp rax
    // clang-format on

    while (s < p + kThunkSize) {
        *s++ = 0xCC;
    }

    VirtualProtect(g_thunks, kMaxThunks * kThunkSize, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), p, kThunkSize);
    ++g_thunksUsed;
    return p;
}

} // namespace

void Gui_SetCallbackLayout(const GuiCallbackLayout& layout)
{
    g_layout = layout;
}

void Gui_SetCrashCallback(void (*onCrash)(HMODULE owner))
{
    g_onCrash = onCrash;
}

void* Gui_CallbackThunk(HMODULE owner, DK2ML_GuiCallbackFn fn, void* user)
{
    if (!fn) {
        return nullptr;
    }
    AcquireSRWLockExclusive(&g_lock);
    if (std::find(g_faulted.begin(), g_faulted.end(), owner) != g_faulted.end()) {
        // A switched-off plugin's thread may still call this; its thunks would be inert.
        ReleaseSRWLockExclusive(&g_lock);
        LogF("%ls: GuiSetCallback refused: it was switched off for this session", Log_ModuleName(owner).c_str());
        return nullptr;
    }

    void* thunk = nullptr;
    for (const Entry& e : g_entries) {
        if (e.owner == owner && e.fn == fn && e.user == user) {
            thunk = e.thunk;
        }
    }
    if (!thunk) {
        g_entries.push_back({fn, user, owner, nullptr});
        thunk = WriteThunk(&g_entries.back());
        if (thunk) {
            g_entries.back().thunk = thunk;
        } else {
            g_entries.pop_back();
        }
    }
    ReleaseSRWLockExclusive(&g_lock);

    if (!thunk) {
        LogF("%ls: GuiSetCallback refused: no room for more callbacks (%zu)", Log_ModuleName(owner).c_str(),
             kMaxThunks);
    }
    return thunk;
}

bool Gui_IsThunk(const void* address)
{
    auto a = static_cast<const uint8_t*>(address);
    return g_thunks && a >= g_thunks && a < g_thunks + kMaxThunks * kThunkSize;
}

int Gui_CallbackCount(HMODULE owner)
{
    AcquireSRWLockShared(&g_lock);
    int n = 0;
    for (const Entry& e : g_entries) {
        n += e.owner == owner;
    }
    ReleaseSRWLockShared(&g_lock);
    return n;
}

void Gui_FaultOwner(HMODULE owner)
{
    AcquireSRWLockExclusive(&g_lock);
    if (std::find(g_faulted.begin(), g_faulted.end(), owner) == g_faulted.end()) {
        g_faulted.push_back(owner);
    }
    g_capturing.erase(std::remove(g_capturing.begin(), g_capturing.end(), owner), g_capturing.end());
    ReleaseSRWLockExclusive(&g_lock);
}

DK2ML_Status Gui_SetCapture(HMODULE owner, bool capture)
{
    AcquireSRWLockExclusive(&g_lock);
    bool faulted = std::find(g_faulted.begin(), g_faulted.end(), owner) != g_faulted.end();
    auto it = std::find(g_capturing.begin(), g_capturing.end(), owner);
    if (capture && !faulted && it == g_capturing.end()) {
        g_capturing.push_back(owner);
    }
    if (!capture && it != g_capturing.end()) {
        g_capturing.erase(it);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return capture && faulted ? DK2ML_ERROR : DK2ML_OK;
}

bool Gui_AnyCapture()
{
    AcquireSRWLockShared(&g_lock);
    bool any = !g_capturing.empty();
    ReleaseSRWLockShared(&g_lock);
    return any;
}

bool Gui_Captures(HMODULE owner)
{
    AcquireSRWLockShared(&g_lock);
    bool mine = std::find(g_capturing.begin(), g_capturing.end(), owner) != g_capturing.end();
    ReleaseSRWLockShared(&g_lock);
    return mine;
}
