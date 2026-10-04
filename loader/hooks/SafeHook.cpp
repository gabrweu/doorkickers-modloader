// Register-preserving hooks (DK2ML_API::CreateSafeHook), see "safe hooks" in dk2ml.h.
//
// MinHook redirects the target to a small per-target stub, which pushes the target's record and jumps to
// SafeHookEntry (SafeHookEntry.asm). That saves every register, calls SafeHook_Pre, restores every register and either
// `ret`s into the MinHook trampoline (the original runs with the caller's exact registers and stack) or back to the
// caller. For post callbacks, SafeHook_Pre swaps the return address for SafeHookPostEntry and keeps the real one on a
// thread-local side stack, so the original's stack frame and stack arguments stay exactly as the caller built them.
//
// Several plugins (and the loader) may hook the same function. MinHook allows one hook per target, so each target
// has one record with a chain of callbacks of any length. Pre callbacks run in creation order, post callbacks in
// reverse order. The first pre that skips the original ends the chain, and the posts of the callbacks before it run
// right away. Each callback has its own scratch[] and its own on/off switch.
//
// The chain is an array of pointers to Callbacks that never move. It grows by publishing a bigger copy of the array
// before the new count, and old arrays are never freed, so the hook path reads it without a lock. A reader that sees
// the new count also sees the new array, because x64 keeps stores and loads in order and MSVC's volatile is
// acquire/release. Flags written to a Callback reach calls already running with an older array.
//
// The posts a call owes are kept on a per-thread pool: a pre that has a post takes the next entry after it runs.
// Nested hooked calls (from callbacks or the original) push and pop above it, so a call's entries stay together until
// its posts have run.
//
// Limitation: the swapped return address has no unwind info, so a C++ exception or stack walk passing through a
// hooked function that has a post callback can't unwind past it. Checked against the exe's unwind table (build 112):
// the game's own code has no C++ try/catch at all, and its only __try (OS_CreateThread's thread naming) is local, so
// no handler sits above a hooked game function. Two costs remain. A crash below a post-hooked function can't be walked
// back to the thread's top-level handler, so the game's crash report may not appear for it. And if a callback calls a
// game function that crashes below a post-hooked call, the loader's own handler isn't reached either.
//
// The side stack is kMaxDepth calls deep per thread, and the post pool kPoolSize entries. When either is full
// (recursion through hooked functions), callbacks that have a post are skipped for that call, pre included, so a pre
// never runs without its post. Pre-only callbacks still run.
//
// Crash containment: callbacks run under __try/__except in SafeHook_Pre/Post. Those have unwind info, so the unwind
// stops there and never reaches the asm frames. A callback that faults gets its registers restored from before the
// call, and every callback of the same plugin passes through from then on.
#include "Loader.h"

#include <malloc.h> // _resetstkoflw

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <vector>

#include "MinHook.h"

namespace {

struct Callback {
    DK2ML_PreFn pre;
    DK2ML_PostFn post;
    void* user;
    HMODULE owner;            // the plugin that created it
    volatile bool enabled;    // EnableHook/DisableHook by its owner
    volatile bool faulted;    // its plugin was switched off (crash, failed init, Mods menu): pass through
    volatile bool skipLogged; // "skipped the original" was logged for it
};

} // namespace

struct SafeHookRecord {
    void* trampoline; // first: SafeHookEntry reads it
    void* target;
    bool hooked;      // the MinHook hook is enabled
    Callback** volatile callbacks; // the chain; replaced by a bigger copy when full, never freed
    volatile int count;            // published after the callback and the array
    int capacity;
};

static_assert(offsetof(SafeHookRecord, trampoline) == 0, "SafeHookEntry.asm reads the trampoline at offset 0");
static_assert(offsetof(DK2ML_Regs, rflags) == 120, "DK2ML_Regs layout must match SafeHookEntry.asm");
static_assert(offsetof(DK2ML_Regs, xmm) == 128, "DK2ML_Regs layout must match SafeHookEntry.asm");
static_assert(offsetof(DK2ML_Regs, stack) == 384, "DK2ML_Regs layout must match SafeHookEntry.asm");
static_assert(offsetof(DK2ML_Regs, scratch) == 392, "DK2ML_Regs layout must match SafeHookEntry.asm");
static_assert(sizeof(DK2ML_Regs) <= 432, "DK2ML_Regs layout must match SafeHookEntry.asm");

extern "C" void SafeHookEntry();
extern "C" void SafeHookPostEntry();

namespace {

// a post a call owes: whose, and the scratch its pre left
struct PostEntry {
    Callback* callback;
    uint64_t scratch[4];
};

// a call whose return address was swapped: its posts are pool entries [first, first + count)
struct PostFrame {
    uint64_t returnAddress;
    SafeHookRecord* record;
    int first, count;
};

constexpr int kMaxDepth = 64;   // calls with posts pending, per thread
constexpr int kPoolSize = 512;  // posts pending, per thread (20 KB)

// plain POD, so no TLS initialization runs
thread_local PostFrame t_frames[kMaxDepth];
thread_local int t_depth;
thread_local PostEntry t_pool[kPoolSize];
thread_local int t_poolTop;
thread_local bool t_overflowLogged;

constexpr size_t kStubSize = 32; // one stub's slot on the stub page
uint8_t* g_stubPage = nullptr;
size_t g_stubPageUsed = 0;
size_t g_stubPageSize = 0;

// push rax / mov rax, record / xchg [rsp], rax / jmp qword ptr [rip+0] / <SafeHookEntry>
// The page is writable only while a stub is written, because antivirus and anti-cheat heuristics flag memory that
// stays writable and executable.
void* MakeStub(SafeHookRecord* record)
{
    DWORD old;
    if (!g_stubPage || g_stubPageUsed + kStubSize > g_stubPageSize) {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        g_stubPageSize = si.dwPageSize;
        g_stubPage =
            static_cast<uint8_t*>(VirtualAlloc(nullptr, g_stubPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        g_stubPageUsed = 0;
        if (!g_stubPage) {
            return nullptr;
        }
    } else if (!VirtualProtect(g_stubPage, g_stubPageSize, PAGE_EXECUTE_READWRITE, &old)) {
        return nullptr; // still executable while writing: stubs already on the page may be running
    }

    uint8_t* p = g_stubPage + g_stubPageUsed;
    g_stubPageUsed += kStubSize;

    uint8_t* s = p;
    uint64_t rec = reinterpret_cast<uint64_t>(record);
    uint64_t entry = reinterpret_cast<uint64_t>(&SafeHookEntry);
    // clang-format off
    *s++ = 0x50;                                          // push rax
    *s++ = 0x48; *s++ = 0xB8;                             // mov rax, imm64
    memcpy(s, &rec, 8); s += 8;                           //   record
    *s++ = 0x48; *s++ = 0x87; *s++ = 0x04; *s++ = 0x24;   // xchg [rsp], rax
    *s++ = 0xFF; *s++ = 0x25;                             // jmp qword ptr [rip+0]
    *s++ = 0; *s++ = 0; *s++ = 0; *s++ = 0;               //   (rel32 = 0)
    memcpy(s, &entry, 8); s += 8;                         //   SafeHookEntry
    // clang-format on

    while (s < p + kStubSize) {
        *s++ = 0xCC;
    }

    if (!VirtualProtect(g_stubPage, g_stubPageSize, PAGE_EXECUTE_READ, &old)) {
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), p, kStubSize);
    return p;
}

std::vector<SafeHookRecord*> g_records; // one per hooked target, never freed (a stub may still point at it)
// Guards g_records and the chains' layout (create, enable/disable, fault). The hook path takes no lock (see the top
// of the file).
SRWLOCK g_recordsLock = SRWLOCK_INIT;

struct ExclusiveLock {
    ExclusiveLock() { AcquireSRWLockExclusive(&g_recordsLock); }

    ~ExclusiveLock() { ReleaseSRWLockExclusive(&g_recordsLock); }
};

std::string (*g_targetName)(void*) = nullptr; // for log lines; the loader names targets from the PDB
SafeHookFaultFn g_onFault = nullptr;

thread_local DWORD t_faultCode;
thread_local void* t_faultAddress;

SafeHookRecord* FindRecord(void* target)
{
    for (SafeHookRecord* r : g_records) {
        if (r->target == target) {
            return r;
        }
    }
    return nullptr;
}

int FaultFilter(EXCEPTION_POINTERS* info)
{
    t_faultCode = info->ExceptionRecord->ExceptionCode;
    t_faultAddress = info->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}

void ContainFault(SafeHookRecord* record, const Callback& c, const char* where)
{
    if (t_faultCode == EXCEPTION_STACK_OVERFLOW) {
        _resetstkoflw();
    }
    LogF("safe hook %p: %s callback crashed (exception 0x%08lX at %p); its plugin's hooks pass through from now on",
         record->target, where, t_faultCode, t_faultAddress);

    SafeHook_FaultOwner(c.owner);
    if (g_onFault) {
        g_onFault(c.owner, t_faultCode, t_faultAddress);
    }
}

// No destructors in here (__try).
bool CallPre(const Callback& c, DK2ML_Regs* regs, int* result)
{
    __try {
        *result = c.pre(regs, c.user);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}

bool CallPost(const Callback& c, DK2ML_Regs* regs)
{
    __try {
        c.post(regs, c.user);
        return true;
    } __except (FaultFilter(GetExceptionInformation())) {
        return false;
    }
}

bool Active(const Callback& c)
{
    return c.enabled && !c.faulted;
}

// Runs the posts in pool entries [first, first + count), last first, each with the scratch its pre left. A callback
// whose pre ran gets its post even if its owner disabled it in between, because pre/post pairs set up and restore
// state. Only a crash stops it. The entries stay reserved while the posts run, so hooked calls they make push above.
void RunPosts(SafeHookRecord* record, DK2ML_Regs* regs, int first, int count)
{
    for (int k = first + count - 1; k >= first; --k) {
        const PostEntry& e = t_pool[k];
        const Callback& c = *e.callback;
        if (c.faulted) {
            continue;
        }

        memcpy(regs->scratch, e.scratch, sizeof(regs->scratch));
        DK2ML_Regs before = *regs;
        int depth = t_depth;
        int top = t_poolTop;
        if (!CallPost(c, regs)) {
            t_depth = depth; // see SafeHook_Pre
            t_poolTop = top;
            *regs = before;  // the caller sees the result as it was before this callback
            ContainFault(record, c, "post");
        }
    }
}

std::wstring OwnerName(HMODULE owner)
{
    return Log_ModuleName(owner);
}

// Logs, once per skipping callback, which other plugins' callbacks didn't run because of the skip.
void LogSkip(SafeHookRecord* record, Callback* const* chain, int count, int skipper)
{
    Callback& c = *chain[skipper];
    if (c.skipLogged) {
        return;
    }

    std::wstring others;
    for (int j = skipper + 1; j < count; ++j) {
        const Callback& later = *chain[j];
        if (Active(later) && later.owner != c.owner) {
            std::wstring name = OwnerName(later.owner);
            if (others.find(name) == std::wstring::npos) {
                others += (others.empty() ? L"" : L", ") + name;
            }
        }
    }
    if (others.empty()) {
        return; // no other plugin's callback was skipped
    }

    c.skipLogged = true;
    std::string target = g_targetName ? g_targetName(record->target) : "";
    LogF(
        "%s (%p): %ls skipped the original, so %ls didn't run for that call (said once; their order is the load order)",
        target.empty() ? "safe hook" : target.c_str(), record->target, OwnerName(c.owner).c_str(), others.c_str());
}

} // namespace

// Called by SafeHookEntry. Returns 0 to run the original, 1 to return to the caller with the registers in regs.
extern "C" int SafeHook_Pre(DK2ML_Regs* regs, SafeHookRecord* record)
{
    const int first = t_poolTop;
    // callbacks with a post run only if a side stack frame is free (see the top of the file)
    const bool frameRoom = t_depth < kMaxDepth;
    const int count = record->count; // count first, then the array: see the top of the file
    Callback* const* chain = record->callbacks;

    for (int i = 0; i < count; ++i) {
        Callback& c = *chain[i];
        if (!Active(c)) {
            continue;
        }

        const bool postRoom = frameRoom && t_poolTop < kPoolSize;
        if (c.post && !postRoom) {
            if (!t_overflowLogged) {
                t_overflowLogged = true;
                LogF("safe hook %p: hooked calls nested too deep on this thread (more than %d, or %d posts pending), "
                     "callbacks with a post skip the deeper calls",
                     record->target, kMaxDepth, kPoolSize);
            }
            continue;
        }

        memset(regs->scratch, 0, sizeof(regs->scratch));
        if (c.pre) {
            DK2ML_Regs before = *regs;
            int result = DK2ML_CALL_ORIGINAL;
            int depth = t_depth;
            int top = t_poolTop;
            if (!CallPre(c, regs, &result)) {
                // If the callback crashed inside a post-hooked call it made, that call's frame is still on the side
                // stack, and the next post would return through it. Restoring the depth drops it. This handler may
                // not be reached at all in that case (see the limitation at the top).
                t_depth = depth;
                t_poolTop = top;
                *regs = before; // the rest of the chain and the original see the call as it was
                ContainFault(record, c, "pre");
                continue;
            }
            if (result == DK2ML_SKIP_ORIGINAL) {
                LogSkip(record, chain, count, i);
                // Earlier callbacks already ran their pre and may rely on their post, so their posts run now, with
                // the skipper's result as if the original had returned it.
                RunPosts(record, regs, first, t_poolTop - first);
                t_poolTop = first;
                return 1;
            }
        }
        if (c.post) { // nested calls made by the pre have popped their entries, so this call's entries stay together
            PostEntry& e = t_pool[t_poolTop++];
            e.callback = &c;
            memcpy(e.scratch, regs->scratch, sizeof(e.scratch));
        }
    }

    if (int owed = t_poolTop - first) { // frameRoom was checked above, and nested calls restored t_depth
        PostFrame& f = t_frames[t_depth++];
        f.returnAddress = regs->stack[0];
        f.record = record;
        f.first = first;
        f.count = owed;
        regs->stack[0] = reinterpret_cast<uint64_t>(&SafeHookPostEntry);
    }
    return 0;
}

// Called by SafeHookPostEntry once the original returned. Returns where the original should have returned to.
extern "C" uint64_t SafeHook_Post(DK2ML_Regs* regs)
{
    const PostFrame top = t_frames[--t_depth]; // a copy: posts may make hooked calls that reuse the slot
    RunPosts(top.record, regs, top.first, top.count);
    t_poolTop = top.first;
    return top.returnAddress;
}

DK2ML_Status SafeHook_Create(void* target, DK2ML_PreFn pre, DK2ML_PostFn post, void* user, HMODULE owner)
{
    ExclusiveLock lock;
    SafeHookRecord* record = FindRecord(target);
    if (!record) {
        record = new SafeHookRecord{};
        record->target = target;
        void* stub = MakeStub(record);
        if (!stub) {
            LogF("CreateSafeHook(%p) failed: out of memory for the stub", target);
            delete record;
            return DK2ML_ERROR;
        }
        MH_STATUS s = MH_CreateHook(target, stub, &record->trampoline);
        if (s != MH_OK) {
            // the stub slot is leaked; harmless
            LogF("CreateSafeHook(%p) failed: %s", target, MH_StatusToString(s));
            delete record;
            return DK2ML_ERROR;
        }
        g_records.push_back(record);
    }

    if (record->count == record->capacity) {
        // a bigger copy, published before the count that needs it; the old array stays for calls still reading it
        int capacity = record->capacity ? record->capacity * 2 : 4;
        Callback** grown = new Callback*[capacity]();
        for (int i = 0; i < record->count; ++i) {
            grown[i] = record->callbacks[i];
        }
        record->callbacks = grown;
        record->capacity = capacity;
    }

    record->callbacks[record->count] = new Callback{pre, post, user, owner, false, false, false};
    record->count = record->count + 1; // published last: a running chain never sees a half-written callback
    return DK2ML_OK;
}

int SafeHook_SetEnabled(void* target, HMODULE owner, bool enabled)
{
    ExclusiveLock lock;
    SafeHookRecord* record = FindRecord(target);
    if (!record) {
        return 0;
    }

    bool any = false;
    for (int i = 0; i < record->count; ++i) {
        Callback& c = *record->callbacks[i];
        if (c.owner == owner) {
            c.enabled = enabled;
        }
        any |= c.enabled;
    }

    if (any != record->hooked) {
        MH_STATUS s = any ? MH_EnableHook(target) : MH_DisableHook(target);
        if (s != MH_OK) {
            LogF("%s(%p) failed: %s", any ? "EnableHook" : "DisableHook", target, MH_StatusToString(s));
            return -1;
        }
        record->hooked = any;
    }
    return 1;
}

void SafeHook_Remove(void* target)
{
    ExclusiveLock lock;
    for (size_t i = 0; i < g_records.size(); ++i) {
        if (g_records[i]->target == target) {
            MH_RemoveHook(target);
            g_records.erase(g_records.begin() + i); // the record itself is leaked on purpose (see g_records)
            return;
        }
    }
}

void SafeHook_FaultOwner(HMODULE owner)
{
    // shared: only flags change, and a fault inside a callback must not wait on a thread that's enabling hooks
    AcquireSRWLockShared(&g_recordsLock);
    for (SafeHookRecord* r : g_records) {
        for (int i = 0; i < r->count; ++i) {
            if (r->callbacks[i]->owner == owner) {
                r->callbacks[i]->faulted = true;
            }
        }
    }
    ReleaseSRWLockShared(&g_recordsLock);
}

void SafeHook_SetFaultCallback(SafeHookFaultFn fn)
{
    g_onFault = fn;
}

uintptr_t SafeHook_PostEntryAddress()
{
    return reinterpret_cast<uintptr_t>(&SafeHookPostEntry);
}

int SafeHook_PostFrames(SafeHookPostFrame* out, int max)
{
    // this thread's calls whose return address is SafeHookPostEntry, innermost (the top of t_frames) first. A call
    // whose posts are running has already left t_frames, so the walk doesn't see it during a post callback.
    int n = 0;
    for (int i = t_depth - 1; i >= 0 && n < max; --i, ++n) {
        out[n] = {t_frames[i].returnAddress, t_frames[i].record ? t_frames[i].record->target : nullptr};
    }
    return n;
}

int SafeHook_OwnersOfTry(void* target, HMODULE* out, int max)
{
    // for the crash report: never waits (the crashing thread may be the one holding the lock)
    if (!TryAcquireSRWLockShared(&g_recordsLock)) {
        return -1;
    }

    int n = 0;
    for (SafeHookRecord* r : g_records) {
        if (r->target != target) {
            continue;
        }
        for (int i = 0; i < r->count && n < max; ++i) {
            HMODULE owner = r->callbacks[i]->owner;
            bool known = false;
            for (int k = 0; k < n; ++k) {
                known |= out[k] == owner;
            }
            if (!known) {
                out[n++] = owner;
            }
        }
    }

    ReleaseSRWLockShared(&g_recordsLock);
    return n;
}

void SafeHook_SetTargetNamer(std::string (*namer)(void* target))
{
    g_targetName = namer;
}

std::vector<SafeHookChain> SafeHook_Chains()
{
    std::vector<SafeHookChain> chains;
    AcquireSRWLockShared(&g_recordsLock);
    for (SafeHookRecord* r : g_records) {
        SafeHookChain chain{r->target, {}};
        for (int i = 0; i < r->count; ++i) {
            if (std::find(chain.owners.begin(), chain.owners.end(), r->callbacks[i]->owner) == chain.owners.end()) {
                chain.owners.push_back(r->callbacks[i]->owner);
            }
        }
        chains.push_back(chain);
    }
    ReleaseSRWLockShared(&g_recordsLock);
    return chains;
}
