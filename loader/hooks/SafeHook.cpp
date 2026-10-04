// Register-preserving hooks (CreateSafeHook); see "safe hooks" in dk2ml.h.
//
// MinHook -> per-target stub -> SafeHookEntry.asm: save all registers, SafeHook_Pre, restore, `ret` into the
// trampoline or back to the caller. A post swaps the return address for SafeHookPostEntry; the real one goes on a
// thread-local side stack, so the original's frame and stack arguments stay as the caller built them.
//
// One record per target, one chain of callbacks: pre in creation order, post reversed. A skipping pre ends the chain
// and the posts of earlier callbacks run right away. The chain array only grows (copy published before the count;
// x64 store order + MSVC volatile) and is never freed, so the hook path reads it without a lock.
//
// Owed posts: per-thread pool (kPoolSize) + side stack (kMaxDepth). Nested hooked calls push and pop above a call's
// entries. When full, callbacks with a post skip the call, pre included; pre-only callbacks still run.
//
// Limitation: SafeHookPostEntry has no unwind info. The game has no handler above hooked functions (build 112), but a
// crash below a post-hooked call gets no crash report and no containment.
//
// Callbacks run under __try in SafeHook_Pre/Post, which have unwind info. A fault restores the registers and makes
// every callback of that plugin pass through.
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
    HMODULE owner;
    volatile bool enabled;    // EnableHook/DisableHook by its owner
    volatile bool faulted;    // owner switched off: pass through
    volatile bool skipLogged;
};

} // namespace

struct SafeHookRecord {
    void* trampoline; // first: SafeHookEntry reads it
    void* target;
    bool hooked;      // MinHook hook enabled
    Callback** volatile callbacks; // replaced by a bigger copy when full, never freed
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

// an owed post and the scratch its pre left
struct PostEntry {
    Callback* callback;
    uint64_t scratch[4];
};

// a call with a swapped return address; its posts are pool entries [first, first + count)
struct PostFrame {
    uint64_t returnAddress;
    SafeHookRecord* record;
    int first, count;
};

constexpr int kMaxDepth = 64;   // calls with posts pending, per thread
constexpr int kPoolSize = 512;  // posts pending, per thread (20 KB)

// POD: no TLS initializer
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
// RWX only while writing: AV and anti-cheat heuristics flag pages that stay RWX.
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
        return nullptr; // stays executable: stubs on the page may be running
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

std::vector<SafeHookRecord*> g_records; // never freed: a stub may still point at one
// Guards g_records and chain changes. The hook path takes no lock (see the top of the file).
SRWLOCK g_recordsLock = SRWLOCK_INIT;

struct ExclusiveLock {
    ExclusiveLock() { AcquireSRWLockExclusive(&g_recordsLock); }

    ~ExclusiveLock() { ReleaseSRWLockExclusive(&g_recordsLock); }
};

std::string (*g_targetName)(void*) = nullptr; // PDB names for log lines
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

// Runs pool entries [first, first + count), last first. A pre that ran gets its post even if disabled since; only a
// fault stops it. The entries stay reserved meanwhile, so hooked calls from posts push above them.
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
            *regs = before;  // result as before this callback
            ContainFault(record, c, "post");
        }
    }
}

std::wstring OwnerName(HMODULE owner)
{
    return Log_ModuleName(owner);
}

// Once per skipper: which other plugins' callbacks the skip bypassed.
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
        return;
    }

    c.skipLogged = true;
    std::string target = g_targetName ? g_targetName(record->target) : "";
    LogF(
        "%s (%p): %ls skipped the original, so %ls didn't run for that call (said once; their order is the load order)",
        target.empty() ? "safe hook" : target.c_str(), record->target, OwnerName(c.owner).c_str(), others.c_str());
}

} // namespace

// From SafeHookEntry. 0: run the original; 1: return to the caller with regs.
extern "C" int SafeHook_Pre(DK2ML_Regs* regs, SafeHookRecord* record)
{
    const int first = t_poolTop;
    const bool frameRoom = t_depth < kMaxDepth;
    const int count = record->count; // count before the array (see the top of the file)
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
                // Drops side-stack frames of post-hooked calls the callback crashed inside, if this handler is
                // reached at all (see the limitation at the top).
                t_depth = depth;
                t_poolTop = top;
                *regs = before;
                ContainFault(record, c, "pre");
                continue;
            }
            if (result == DK2ML_SKIP_ORIGINAL) {
                LogSkip(record, chain, count, i);
                // Earlier pres' posts run now, with the skipper's result as the original's.
                RunPosts(record, regs, first, t_poolTop - first);
                t_poolTop = first;
                return 1;
            }
        }
        if (c.post) { // the pre's nested calls have popped their entries
            PostEntry& e = t_pool[t_poolTop++];
            e.callback = &c;
            memcpy(e.scratch, regs->scratch, sizeof(e.scratch));
        }
    }

    if (int owed = t_poolTop - first) { // frameRoom checked above; nested calls restored t_depth
        PostFrame& f = t_frames[t_depth++];
        f.returnAddress = regs->stack[0];
        f.record = record;
        f.first = first;
        f.count = owed;
        regs->stack[0] = reinterpret_cast<uint64_t>(&SafeHookPostEntry);
    }
    return 0;
}

// From SafeHookPostEntry. Returns the real return address.
extern "C" uint64_t SafeHook_Post(DK2ML_Regs* regs)
{
    const PostFrame top = t_frames[--t_depth]; // a copy: hooked calls from posts reuse the slot
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
            // the stub slot leaks
            LogF("CreateSafeHook(%p) failed: %s", target, MH_StatusToString(s));
            delete record;
            return DK2ML_ERROR;
        }
        g_records.push_back(record);
    }

    if (record->count == record->capacity) {
        // published before the count; the old array stays for calls still reading it
        int capacity = record->capacity ? record->capacity * 2 : 4;
        Callback** grown = new Callback*[capacity]();
        for (int i = 0; i < record->count; ++i) {
            grown[i] = record->callbacks[i];
        }
        record->callbacks = grown;
        record->capacity = capacity;
    }

    record->callbacks[record->count] = new Callback{pre, post, user, owner, false, false, false};
    record->count = record->count + 1; // published last
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
            g_records.erase(g_records.begin() + i); // the record leaks (see g_records)
            return;
        }
    }
}

void SafeHook_FaultOwner(HMODULE owner)
{
    // shared: only flags change, and a faulting callback must not wait on a thread enabling hooks
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
    // innermost first. A call whose posts are running has already left t_frames.
    int n = 0;
    for (int i = t_depth - 1; i >= 0 && n < max; --i, ++n) {
        out[n] = {t_frames[i].returnAddress, t_frames[i].record ? t_frames[i].record->target : nullptr};
    }
    return n;
}

int SafeHook_OwnersOfTry(void* target, HMODULE* out, int max)
{
    // crash report: never waits; the crashing thread may hold the lock
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
