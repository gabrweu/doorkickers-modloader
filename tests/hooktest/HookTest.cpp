// Checks that safe hooks (loader/hooks/SafeHook.cpp) keep every register, including the ones the x64 ABI calls
// volatile, because the game's link-time-optimized callers keep values in them across calls. Also checks that the same
// comparison catches a plain C++ detour, so a pass means something.
// usage: hooktest.exe   (prints "all passed", exit code 0)
#include "../../loader/Loader.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "MinHook.h"

using HT_State = DK2ML_Regs; // same layout up to the xmm registers; stack/scratch unused

extern "C" void HT_Call(const HT_State* in, HT_State* out, void* fn);
extern "C" void HT_Nop();
extern "C" void HT_RetRcx();
extern "C" void HT_Recurse();
extern "C" void HT_Clobber();

namespace {

constexpr uint64_t kFlagsMask = 0x8D5; // CF PF AF ZF SF OF (DF must stay 0)

int g_failures = 0;
int g_preCalls = 0;
int g_postCalls = 0;

const char* const kGprNames[] = {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8",
                                 "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

HT_State MakeInput()
{
    HT_State s = {};
    uint64_t* g = &s.rax;
    for (int i = 0; i < 15; ++i) {
        g[i] = 0x1111111111111111ull * (i + 1) ^ 0x0123456789ABCDEFull;
    }

    s.rflags = 0x2 | 0x1 | 0x4 | 0x40 | 0x80 | 0x800; // CF PF ZF SF OF set, so a pass can't be "flags happened to be 0"

    for (int i = 0; i < 16; ++i) {
        s.xmm[i] = {0xA5A5000000000000ull | i, 0x5A5A000000000000ull | (i << 8)};
    }
    return s;
}

// compares every register except the ones listed in `except` (names, "flags", "xmmN")
int Compare(const char* test, const HT_State& want, const HT_State& got, const char* const* except = nullptr)
{
    auto skipped = [&](const char* name) {
        for (auto e = except; e && *e; ++e) {
            if (!strcmp(*e, name)) {
                return true;
            }
        }
        return false;
    };

    int bad = 0;
    const uint64_t* w = &want.rax;
    const uint64_t* g = &got.rax;
    for (int i = 0; i < 15; ++i) {
        if (!skipped(kGprNames[i]) && w[i] != g[i]) {
            printf("  %s: %s %016llx, want %016llx\n", test, kGprNames[i], g[i], w[i]);
            ++bad;
        }
    }

    if (!skipped("flags") && (want.rflags & kFlagsMask) != (got.rflags & kFlagsMask)) {
        printf("  %s: rflags %llx, want %llx\n", test, got.rflags & kFlagsMask, want.rflags & kFlagsMask);
        ++bad;
    }

    for (int i = 0; i < 16; ++i) {
        char name[8];
        sprintf_s(name, "xmm%d", i);
        if (!skipped(name) && memcmp(&want.xmm[i], &got.xmm[i], 16)) {
            printf("  %s: %s %016llx%016llx, want %016llx%016llx\n", test, name, got.xmm[i].hi, got.xmm[i].lo,
                   want.xmm[i].hi, want.xmm[i].lo);
            ++bad;
        }
    }

    return bad;
}

void Expect(const char* test, bool ok, int mismatches)
{
    printf("%-58s %s\n", test, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
    (void)mismatches;
}

HT_State Run(void* fn, const HT_State& in)
{
    HT_State out = {};
    HT_Call(&in, &out, fn);
    return out;
}

// callbacks: each first clobbers every volatile register, like any compiled C++ may
int PrePass(DK2ML_Regs*, void*)
{
    HT_Clobber();
    ++g_preCalls;
    return DK2ML_CALL_ORIGINAL;
}

void PostPass(DK2ML_Regs*, void*)
{
    HT_Clobber();
    ++g_postCalls;
}

int PreSkip(DK2ML_Regs* r, void*)
{
    HT_Clobber();
    r->rax = 0x1234;
    r->xmm[0] = {0x3F800000, 0};
    return DK2ML_SKIP_ORIGINAL;
}

int PreArgs(DK2ML_Regs* r, void*)
{
    HT_Clobber();
    r->rcx = 0x777;
    r->xmm[1] = {0x42, 0x43};
    r->scratch[2] = 0xFEED;

    if (r->stack[0] == 0) {
        printf("  stack[0] (return address) is 0\n");
    }
    return DK2ML_CALL_ORIGINAL;
}

void PostArgs(DK2ML_Regs* r, void*)
{
    HT_Clobber();
    if (r->scratch[2] != 0xFEED) {
        printf("  scratch not carried from pre to post\n");
    }
    r->rax += 1;
}

// fake plugin modules: hooks are grouped by owner, and a crash switches off all hooks of that owner
const HMODULE kPassOwner = reinterpret_cast<HMODULE>(0x10000);
const HMODULE kCrashPreOwner = reinterpret_cast<HMODULE>(0x20000);
const HMODULE kCrashPostOwner = reinterpret_cast<HMODULE>(0x30000);

int g_faults = 0;
HMODULE g_faultOwner = nullptr;

void OnFault(HMODULE owner, DWORD, void*)
{
    ++g_faults;
    g_faultOwner = owner;
}

int PreCrash(DK2ML_Regs* r, void*)
{
    HT_Clobber();
    r->rcx = 0xDEAD; // half-done changes must not reach the original
    *static_cast<volatile int*>(nullptr) = 1;
    return DK2ML_SKIP_ORIGINAL;
}

void PostCrash(DK2ML_Regs* r, void*)
{
    HT_Clobber();
    r->rax = 0xDEAD;
    *static_cast<volatile int*>(nullptr) = 1;
}

void EnableAll(void* target)
{
    for (HMODULE owner : {kPassOwner, kCrashPreOwner, kCrashPostOwner}) {
        SafeHook_SetEnabled(target, owner, true);
    }
}

// chained hooks: two owners on one target
const HMODULE kOwnerA = reinterpret_cast<HMODULE>(0x40000);
const HMODULE kOwnerB = reinterpret_cast<HMODULE>(0x50000);

char g_order[16];
int g_orderLen = 0;
bool g_scratchOk = true;

int PreA(DK2ML_Regs* r, void*)
{
    HT_Clobber();
    g_order[g_orderLen++] = 'a';

    if (r->scratch[0] != 0) {
        g_scratchOk = false; // every callback starts with its own, empty scratch
    }
    r->scratch[0] = 0xA;
    r->rcx += 1; // pre's see the previous pre's argument changes
    return DK2ML_CALL_ORIGINAL;
}

void PostA(DK2ML_Regs* r, void*)
{
    HT_Clobber();
    g_order[g_orderLen++] = 'A';
    if (r->scratch[0] != 0xA) {
        g_scratchOk = false;
    }
    r->rax *= 2;
}

int PreB(DK2ML_Regs* r, void*)
{
    HT_Clobber();
    g_order[g_orderLen++] = 'b';

    if (r->scratch[0] != 0) {
        g_scratchOk = false;
    }
    r->scratch[0] = 0xB;
    r->rcx += 10;
    return DK2ML_CALL_ORIGINAL;
}

void PostB(DK2ML_Regs* r, void*)
{
    HT_Clobber();
    g_order[g_orderLen++] = 'B';
    if (r->scratch[0] != 0xB) {
        g_scratchOk = false;
    }
    r->rax += 100;
}

int PreBSkip(DK2ML_Regs* r, void*)
{
    g_order[g_orderLen++] = 's';
    r->rax = 7;
    return DK2ML_SKIP_ORIGINAL;
}

int PreBCrash(DK2ML_Regs* r, void*)
{
    g_order[g_orderLen++] = 'x';
    r->rcx = 0xDEAD;
    *static_cast<volatile int*>(nullptr) = 1;
    return DK2ML_CALL_ORIGINAL;
}

std::string Order()
{
    std::string s(g_order, g_orderLen);
    g_orderLen = 0;
    return s;
}

// nesting deeper than the side stack: pairs must stay matched
const HMODULE kPairOwner = reinterpret_cast<HMODULE>(0x70000);
const HMODULE kPreOnlyOwner = reinterpret_cast<HMODULE>(0x80000);

int g_pairPre = 0;
int g_pairPost = 0;
int g_preOnly = 0;

int PrePair(DK2ML_Regs*, void*)
{
    HT_Clobber();
    ++g_pairPre;
    return DK2ML_CALL_ORIGINAL;
}

void PostPair(DK2ML_Regs*, void*)
{
    HT_Clobber();
    ++g_pairPost;
}

int PreOnlyCount(DK2ML_Regs*, void*)
{
    HT_Clobber();
    ++g_preOnly;
    return DK2ML_CALL_ORIGINAL;
}

// a pre that switches its own hook off still gets its post for that call
const HMODULE kSelfOwner = reinterpret_cast<HMODULE>(0x60000);

int PreDisableSelf(DK2ML_Regs*, void* target)
{
    HT_Clobber();
    ++g_preCalls;
    SafeHook_SetEnabled(target, kSelfOwner, false);
    return DK2ML_CALL_ORIGINAL;
}

// the argument/result helpers in dk2ml.h. HT_Call keeps `out` and `fn` at [rsp+32] and [rsp+40] when it calls, which
// is where the 5th and 6th arguments go.
bool g_helpersOk = false;
uint64_t g_wantArg4 = 0;
uint64_t g_wantArg5 = 0;

int PreHelpers(DK2ML_Regs* r, void*)
{
    HT_Clobber();

    // plain reads of the saved registers and the caller's stack, so evaluating all of them changes nothing
    bool registerArgs = DK2ML_Arg(r, 0) == 0x1000 && DK2ML_Arg(r, 1) == r->rdx && DK2ML_Arg(r, 3) == r->r9;
    bool floatArg = DK2ML_ArgFloat(r, 1) == 2.5f;
    bool stackArgs = DK2ML_Arg(r, 4) == g_wantArg4 && DK2ML_Arg(r, 5) == g_wantArg5;
    g_helpersOk = registerArgs && floatArg && stackArgs;

    DK2ML_SetArg(r, 0, 0x2000);
    DK2ML_SetArgFloat(r, 1, 4.0f); // HT_RetRcx returns rcx and xmm1
    return DK2ML_CALL_ORIGINAL;
}

void PostHelpers(DK2ML_Regs* r, void*)
{
    HT_Clobber();
    g_helpersOk = g_helpersOk && r->rax == 0x2000 && DK2ML_ResultFloat(r) == 4.0f;
    DK2ML_SetResult(r, r->rax + 1);
    DK2ML_SetResultFloat(r, DK2ML_ResultFloat(r) * 2);
}

// long chains: owner i has callbacks with user = i
constexpr int kChain = 40;
int g_chainPre[kChain];
int g_chainPost[kChain];
int g_chainPreOnly;
std::vector<int> g_chainSeq; // i for owner i's pre, -(i + 1) for its post
bool g_chainScratchOk = true;
int g_skipAt = -1;

HMODULE ChainOwner(int i)
{
    return reinterpret_cast<HMODULE>(0x1000000 + 0x10000 * static_cast<uintptr_t>(i));
}

void ResetChain()
{
    memset(g_chainPre, 0, sizeof(g_chainPre));
    memset(g_chainPost, 0, sizeof(g_chainPost));
    g_chainPreOnly = 0;
    g_chainSeq.clear();
    g_chainScratchOk = true;
}

int PreChain(DK2ML_Regs* r, void* user)
{
    HT_Clobber();
    int i = static_cast<int>(reinterpret_cast<intptr_t>(user));
    ++g_chainPre[i];
    g_chainSeq.push_back(i);

    if (r->scratch[0] != 0) {
        g_chainScratchOk = false; // each callback starts with its own, empty scratch
    }
    r->scratch[0] = 1000 + i;
    r->rcx += 1;

    if (i == g_skipAt) {
        r->rax = 7;
        return DK2ML_SKIP_ORIGINAL;
    }
    return DK2ML_CALL_ORIGINAL;
}

void PostChain(DK2ML_Regs* r, void* user)
{
    HT_Clobber();
    int i = static_cast<int>(reinterpret_cast<intptr_t>(user));
    ++g_chainPost[i];
    g_chainSeq.push_back(-(i + 1));

    if (r->scratch[0] != static_cast<uint64_t>(1000 + i)) {
        g_chainScratchOk = false;
    }
}

// for recursion: count only, leave rcx (the recursion counter) alone
int PreCount(DK2ML_Regs* r, void* user)
{
    HT_Clobber();
    int i = static_cast<int>(reinterpret_cast<intptr_t>(user));
    ++g_chainPre[i];
    r->scratch[0] = 2000 + i;
    return DK2ML_CALL_ORIGINAL;
}

void PostCount(DK2ML_Regs* r, void* user)
{
    HT_Clobber();
    int i = static_cast<int>(reinterpret_cast<intptr_t>(user));
    ++g_chainPost[i];

    if (r->scratch[0] != static_cast<uint64_t>(2000 + i)) {
        g_chainScratchOk = false; // a nested level's entries must not mix with this one's
    }
}

int PreOnlyChain(DK2ML_Regs*, void*)
{
    HT_Clobber();
    ++g_chainPreOnly;
    return DK2ML_CALL_ORIGINAL;
}

std::vector<int> ExpectedSeq(int pres, int posts) // pres 0..pres-1, then posts posts-1..0
{
    std::vector<int> seq;
    for (int i = 0; i < pres; ++i) {
        seq.push_back(i);
    }

    for (int i = posts - 1; i >= 0; --i) {
        seq.push_back(-(i + 1));
    }
    return seq;
}

int CountInLog(const char* text)
{
    std::ifstream log("hooktest.log");
    std::string line;
    int n = 0;
    while (std::getline(log, line)) {
        n += line.find(text) != std::string::npos;
    }
    return n;
}

void* g_plainOriginal = nullptr;

// an ordinary C++ detour; it clobbers volatile registers that the caller keeps across the call
extern "C" void PlainDetour()
{
    HT_Clobber();
    reinterpret_cast<void (*)()>(g_plainOriginal)();
}

// baseline: the harness itself, and a plain detour that must be caught, or the test proves nothing
void TestBaseline(const HT_State& in)
{
    int n = Compare("unhooked nop", in, Run(&HT_Nop, in));
    Expect("unhooked nop keeps everything", n == 0, n);

    MH_CreateHook(&HT_Nop, &PlainDetour, &g_plainOriginal);
    MH_EnableHook(&HT_Nop);
    printf("(expected mismatches from a plain C++ detour:)\n");
    n = Compare("plain detour", in, Run(&HT_Nop, in));
    Expect("plain C++ detour is detected as clobbering", n > 0, n);
    MH_RemoveHook(&HT_Nop);
}

// one safe hook at a time: pass-through, skip, argument and result changes, recursion
void TestSingleHooks(const HT_State& in)
{
    int n;

    // pass-through, pre + post
    SafeHook_Create(&HT_Nop, PrePass, PostPass, nullptr, kPassOwner);
    EnableAll(&HT_Nop);
    g_preCalls = g_postCalls = 0;
    n = Compare("safe pass-through", in, Run(&HT_Nop, in));
    Expect("safe hook, pre+post pass-through keeps everything", n == 0 && g_preCalls == 1 && g_postCalls == 1, n);
    SafeHook_Remove(&HT_Nop);

    // pre only
    SafeHook_Create(&HT_Nop, PrePass, nullptr, nullptr, kPassOwner);
    EnableAll(&HT_Nop);
    n = Compare("safe pre only", in, Run(&HT_Nop, in));
    Expect("safe hook, pre only keeps everything", n == 0, n);
    SafeHook_Remove(&HT_Nop);

    // post only
    SafeHook_Create(&HT_Nop, nullptr, PostPass, nullptr, kPassOwner);
    EnableAll(&HT_Nop);
    n = Compare("safe post only", in, Run(&HT_Nop, in));
    Expect("safe hook, post only keeps everything", n == 0, n);
    SafeHook_Remove(&HT_Nop);

    // skip with a return value
    SafeHook_Create(&HT_Nop, PreSkip, PostPass, nullptr, kPassOwner);
    EnableAll(&HT_Nop);
    g_postCalls = 0;
    {
        HT_State out = Run(&HT_Nop, in);
        const char* ex[] = {"rax", "xmm0", nullptr};
        n = Compare("safe skip", in, out, ex);

        bool xmm0Ok = out.xmm[0].lo == 0x3F800000 && out.xmm[0].hi == 0;
        bool ok = n == 0 && out.rax == 0x1234 && xmm0Ok && g_postCalls == 0;
        Expect("safe hook, skip returns rax/xmm0, keeps the rest", ok, n);
    }
    SafeHook_Remove(&HT_Nop);

    // changing arguments, changing the result in post
    SafeHook_Create(&HT_RetRcx, PreArgs, PostArgs, nullptr, kPassOwner);
    EnableAll(&HT_RetRcx);
    {
        HT_State out = Run(&HT_RetRcx, in);
        HT_State want = in;
        want.rcx = 0x777;
        want.xmm[1] = {0x42, 0x43};
        const char* ex[] = {"rax", "xmm0", nullptr};
        n = Compare("safe args", want, out, ex);

        bool ok = n == 0 && out.rax == 0x778 && out.xmm[0].lo == 0x42 && out.xmm[0].hi == 0x43;
        Expect("safe hook, pre changes args, post changes result", ok, n);
    }
    SafeHook_Remove(&HT_RetRcx);

    // recursion through the hook: each level gets its own post frame
    SafeHook_Create(&HT_Recurse, PrePass, PostPass, nullptr, kPassOwner);
    EnableAll(&HT_Recurse);
    {
        HT_State rin = in;
        rin.rcx = 5;
        g_preCalls = g_postCalls = 0;
        HT_State out = Run(&HT_Recurse, rin);
        const char* ex[] = {"rax", "flags", nullptr};
        n = Compare("safe recursion", rin, out, ex);

        bool ok = n == 0 && out.rax == 5 && g_preCalls == 6 && g_postCalls == 6;
        if (!ok) {
            printf("  rax %llu, pre %d, post %d\n", out.rax, g_preCalls, g_postCalls);
        }
        Expect("safe hook, recursion (6 levels) keeps everything", ok, n);
    }
    SafeHook_Remove(&HT_Recurse);
}

// crash containment: a crashing callback leaves the call as the caller set it up and switches off its plugin
void TestCrashContainment(const HT_State& in)
{
    int n;

    // a pre that crashes leaves the call untouched, and every hook of its plugin passes through
    SafeHook_SetFaultCallback(OnFault);
    SafeHook_Create(&HT_Nop, PreCrash, PostPass, nullptr, kCrashPreOwner);
    SafeHook_Create(&HT_RetRcx, PrePass, PostPass, nullptr, kCrashPreOwner); // same plugin, never crashes itself
    EnableAll(&HT_Nop);
    EnableAll(&HT_RetRcx);
    {
        g_preCalls = g_postCalls = 0;
        n = Compare("safe pre crash", in, Run(&HT_Nop, in));
        Expect("crashing pre: original runs as the caller set it up",
               n == 0 && g_faults == 1 && g_faultOwner == kCrashPreOwner, n);
        Expect("crashing pre: its post is skipped", g_postCalls == 0, 0);

        n = Compare("safe pre crash, again", in, Run(&HT_Nop, in));
        Expect("crashed hook passes through on the next call", n == 0 && g_faults == 1, n);

        HT_State out = Run(&HT_RetRcx, in);
        const char* ex[] = {"rax", "xmm0", nullptr};
        n = Compare("safe pre crash, sibling", in, out, ex);
        Expect("the plugin's other hooks pass through too",
               n == 0 && g_preCalls == 0 && g_postCalls == 0 && out.rax == in.rcx, n);
    }
    SafeHook_Remove(&HT_Nop);
    SafeHook_Remove(&HT_RetRcx);

    // a post that crashes: the caller still gets the original's result and registers
    SafeHook_Create(&HT_RetRcx, PrePass, PostCrash, nullptr, kCrashPostOwner);
    EnableAll(&HT_RetRcx);
    {
        g_faults = 0;
        HT_State out = Run(&HT_RetRcx, in);
        const char* ex[] = {"rax", "xmm0", nullptr};
        n = Compare("safe post crash", in, out, ex);

        bool resultOk = out.rax == in.rcx && !memcmp(&out.xmm[0], &in.xmm[1], 16);
        bool faultOk = g_faults == 1 && g_faultOwner == kCrashPostOwner;
        bool ok = n == 0 && resultOk && faultOk;
        Expect("crashing post: caller gets the original's result", ok, n);
    }
    SafeHook_Remove(&HT_RetRcx);
}

// chains: two plugins on one function
void TestChains(const HT_State& in)
{
    int n;

    const char* exResult[] = {"rax", "rcx", "xmm0", nullptr};
    SafeHook_Create(&HT_RetRcx, PreA, PostA, nullptr, kOwnerA);
    SafeHook_Create(&HT_RetRcx, PreB, PostB, nullptr, kOwnerB);
    SafeHook_SetEnabled(&HT_RetRcx, kOwnerA, true);
    SafeHook_SetEnabled(&HT_RetRcx, kOwnerB, true);
    {
        HT_State rin = in;
        rin.rcx = 1;
        HT_State out = Run(&HT_RetRcx, rin);
        n = Compare("chain", rin, out, exResult);

        // original returns rcx = 1 + 1 + 10 = 12; posts in reverse: B (+100) then A (*2) = 224
        std::string order = Order();
        bool ok = n == 0 && order == "abBA" && out.rax == 224 && g_scratchOk;
        if (!ok) {
            printf("  order %s, rax %llu, scratch %s\n", order.c_str(), out.rax, g_scratchOk ? "ok" : "mixed up");
        }
        Expect("chain: pre's in order, post's reversed, own scratch each", ok, n);

        SafeHook_SetEnabled(&HT_RetRcx, kOwnerA, false);
        out = Run(&HT_RetRcx, rin);
        order = Order();
        Expect("chain: disabling one plugin leaves the other running", order == "bB" && out.rax == 111, 0);

        SafeHook_SetEnabled(&HT_RetRcx, kOwnerB, false);
        out = Run(&HT_RetRcx, rin);
        Expect("chain: all disabled = unhooked", Order().empty() && out.rax == 1, 0);
    }
    SafeHook_Remove(&HT_RetRcx);

    SafeHook_Create(&HT_RetRcx, PreA, PostA, nullptr, kOwnerA);
    SafeHook_Create(&HT_RetRcx, PreBSkip, PostB, nullptr, kOwnerB);
    SafeHook_Create(&HT_RetRcx, PreA, PostA, nullptr, kOwnerA); // after the skipper: never runs
    SafeHook_SetEnabled(&HT_RetRcx, kOwnerA, true);
    SafeHook_SetEnabled(&HT_RetRcx, kOwnerB, true);
    {
        HT_State out = Run(&HT_RetRcx, in);
        std::string order = Order();

        // the skipper's own post and everything after it don't run; A's pre ran, so A's post gets the skipper's
        // result (7 * 2)
        bool ok = order == "asA" && out.rax == 14 && g_scratchOk;
        if (!ok) {
            printf("  order %s, rax %llu\n", order.c_str(), out.rax);
        }
        Expect("chain: a skip ends the chain, earlier post's still run", ok, 0);
    }
    SafeHook_Remove(&HT_RetRcx);

    // a skip on a single hook with a pre-only skipper and a post-only callback before it: the registers the caller
    // sees are the skipper's, with every other register kept
    SafeHook_Create(&HT_Nop, nullptr, PostPass, nullptr, kOwnerA);
    SafeHook_Create(&HT_Nop, PreSkip, nullptr, nullptr, kOwnerB);
    SafeHook_SetEnabled(&HT_Nop, kOwnerA, true);
    SafeHook_SetEnabled(&HT_Nop, kOwnerB, true);
    {
        g_postCalls = 0;
        HT_State out = Run(&HT_Nop, in);
        const char* ex[] = {"rax", "xmm0", nullptr};
        n = Compare("chain skip regs", in, out, ex);
        Expect("chain: skip with an earlier post keeps everything else",
               n == 0 && out.rax == 0x1234 && g_postCalls == 1, n);
    }
    SafeHook_Remove(&HT_Nop);

    g_faults = 0;
    SafeHook_Create(&HT_RetRcx, PreA, PostA, nullptr, kOwnerA);
    SafeHook_Create(&HT_RetRcx, PreBCrash, PostB, nullptr, kOwnerB);
    SafeHook_SetEnabled(&HT_RetRcx, kOwnerA, true);
    SafeHook_SetEnabled(&HT_RetRcx, kOwnerB, true);
    {
        HT_State rin = in;
        rin.rcx = 1;
        HT_State out = Run(&HT_RetRcx, rin);
        std::string order = Order();

        // A's change (rcx 2) survives, B's half-done change doesn't; B's post is skipped, A's runs: 2 * 2
        bool ok = order == "axA" && out.rax == 4 && g_faults == 1 && g_faultOwner == kOwnerB;
        if (!ok) {
            printf("  order %s, rax %llu, faults %d\n", order.c_str(), out.rax, g_faults);
        }
        Expect("chain: one plugin crashing leaves the other running", ok, 0);

        out = Run(&HT_RetRcx, rin);
        Expect("chain: the crashed plugin stays out", Order() == "aA" && out.rax == 4, 0);
    }
    SafeHook_Remove(&HT_RetRcx);
}

// pre/post pairs stay matched: deeper than the side stack, and when a pre switches its own hook off
void TestMatchedPairs(const HT_State& in)
{
    int n;

    SafeHook_Create(&HT_Recurse, PrePair, PostPair, nullptr, kPairOwner);
    SafeHook_Create(&HT_Recurse, PreOnlyCount, nullptr, nullptr, kPreOnlyOwner);
    SafeHook_SetEnabled(&HT_Recurse, kPairOwner, true);
    SafeHook_SetEnabled(&HT_Recurse, kPreOnlyOwner, true);
    {
        HT_State rin = in;
        rin.rcx = 70; // 71 nested calls, the side stack holds 64
        HT_State out = Run(&HT_Recurse, rin);
        const char* ex[] = {"rax", "flags", nullptr};
        n = Compare("safe deep recursion", rin, out, ex);

        bool countsOk = g_pairPre == 64 && g_pairPost == 64 && g_preOnly == 71;
        bool ok = n == 0 && out.rax == 70 && countsOk;
        if (!ok) {
            printf("  rax %llu, pair pre %d post %d, pre-only %d\n", out.rax, g_pairPre, g_pairPost, g_preOnly);
        }
        Expect("deeper than the side stack: no pre without its post", ok, n);

        g_pairPre = g_pairPost = g_preOnly = 0;
        rin.rcx = 3;
        Run(&HT_Recurse, rin);
        Expect("after that, every level gets its pair again", g_pairPre == 4 && g_pairPost == 4 && g_preOnly == 4, 0);
    }
    SafeHook_Remove(&HT_Recurse);

    SafeHook_Create(&HT_RetRcx, PreDisableSelf, PostPass, &HT_RetRcx, kSelfOwner);
    SafeHook_SetEnabled(&HT_RetRcx, kSelfOwner, true);
    {
        g_preCalls = g_postCalls = 0;
        HT_State out = Run(&HT_RetRcx, in);
        const char* ex[] = {"rax", "xmm0", nullptr};
        n = Compare("safe disable in pre", in, out, ex);
        Expect("a pre that disables its hook still gets its post",
               n == 0 && out.rax == in.rcx && g_preCalls == 1 && g_postCalls == 1, n);

        out = Run(&HT_RetRcx, in);
        Expect("and the next call is unhooked", out.rax == in.rcx && g_preCalls == 1 && g_postCalls == 1, 0);
    }
    SafeHook_Remove(&HT_RetRcx);
}

// the argument/result helpers in dk2ml.h
void TestHelpers(const HT_State& in)
{
    SafeHook_Create(&HT_RetRcx, PreHelpers, PostHelpers, nullptr, kPassOwner);
    SafeHook_SetEnabled(&HT_RetRcx, kPassOwner, true);
    {
        HT_State rin = in;
        rin.rcx = 0x1000;
        rin.xmm[1].lo = 0xA5A5A5A540200000ull; // 2.5f under unrelated upper bits, which must be ignored and kept
        HT_State out = {};
        g_wantArg4 = reinterpret_cast<uint64_t>(&out);
        g_wantArg5 = reinterpret_cast<uint64_t>(&HT_RetRcx);
        HT_Call(&rin, &out, &HT_RetRcx);

        // 4.0f doubled = 8.0f (0x41000000), upper bits as they came in
        bool ok = g_helpersOk && out.rax == 0x2001 && out.xmm[0].lo == 0xA5A5A5A541000000ull;
        if (!ok) {
            printf("  helpers %s, rax %llx, xmm0 %016llx\n", g_helpersOk ? "ok" : "wrong", out.rax, out.xmm[0].lo);
        }
        Expect("DK2ML_Arg/SetArg/ArgFloat/SetResult helpers", ok, 0);
    }
    SafeHook_Remove(&HT_RetRcx);
}

// long chains: no fixed limit; switches, order, scratch and skips still work with 40 owners
void TestLongChain(const HT_State& in)
{
    int n;
    const char* ex[] = {"rax", "rcx", "xmm0", nullptr};
    HT_State rin = in;
    rin.rcx = 1;

    // 10 owners, one switched off, then 30 more: the chain's array is replaced while it grows
    for (int i = 0; i < 10; ++i) {
        SafeHook_Create(&HT_RetRcx, PreChain, PostChain, reinterpret_cast<void*>(static_cast<intptr_t>(i)),
                        ChainOwner(i));
        SafeHook_SetEnabled(&HT_RetRcx, ChainOwner(i), true);
    }
    SafeHook_SetEnabled(&HT_RetRcx, ChainOwner(3), false);

    bool created = true;
    for (int i = 10; i < kChain; ++i) {
        created &= SafeHook_Create(&HT_RetRcx, PreChain, PostChain, reinterpret_cast<void*>(static_cast<intptr_t>(i)),
                                   ChainOwner(i)) == DK2ML_OK;
        SafeHook_SetEnabled(&HT_RetRcx, ChainOwner(i), true);
    }
    SafeHook_SetEnabled(&HT_RetRcx, ChainOwner(5), false); // an owner from before the chain grew

    ResetChain();
    HT_State out = Run(&HT_RetRcx, rin);
    bool switchedOffSkipped = g_chainPre[3] == 0 && g_chainPre[5] == 0 && g_chainPost[3] == 0;
    bool othersRan = g_chainPre[4] == 1 && g_chainPre[39] == 1 && g_chainPost[39] == 1;
    bool togglesOk = created && switchedOffSkipped && othersRan && out.rax == 1 + kChain - 2;
    Expect("40 hooks on one function; switches still work as it grows", togglesOk, 0);

    SafeHook_SetEnabled(&HT_RetRcx, ChainOwner(3), true);
    SafeHook_SetEnabled(&HT_RetRcx, ChainOwner(5), true);
    ResetChain();
    out = Run(&HT_RetRcx, rin);
    n = Compare("long chain", rin, out, ex);
    bool ok = n == 0 && g_chainSeq == ExpectedSeq(kChain, kChain) && g_chainScratchOk && out.rax == 1 + kChain;
    if (!ok) {
        printf("  %zu calls, scratch %s, rax %llu\n", g_chainSeq.size(), g_chainScratchOk ? "ok" : "mixed", out.rax);
    }
    Expect("40 hooks: pre's in order, post's reversed, own scratch each", ok, n);

    // owner 20 skips: owners 0-20 ran their pre, 0-19 get their post (with 7), 21-39 don't run
    g_skipAt = 20;
    int skipLines = CountInLog("skipped the original"); // the earlier chain test's skip has its own line
    ResetChain();
    out = Run(&HT_RetRcx, rin);
    ok = g_chainSeq == ExpectedSeq(21, 20) && out.rax == 7 && g_chainScratchOk;
    Expect("40 hooks: a skip at #20 runs posts 19..0 only", ok, 0);

    Run(&HT_RetRcx, rin);
    Expect("the skip is logged once, naming who missed out", CountInLog("skipped the original") == skipLines + 1, 0);

    g_skipAt = -1;
    SafeHook_Remove(&HT_RetRcx);
}

// 40 pairs per level and 21 levels: the 512-entry pool fills at level 12, where only 32 of the 40 fit
void TestPoolFull(const HT_State& in)
{
    for (int i = 0; i < kChain; ++i) {
        SafeHook_Create(&HT_Recurse, PreCount, PostCount, reinterpret_cast<void*>(static_cast<intptr_t>(i)),
                        ChainOwner(i));
        SafeHook_SetEnabled(&HT_Recurse, ChainOwner(i), true);
    }
    SafeHook_Create(&HT_Recurse, PreOnlyChain, nullptr, nullptr, kPreOnlyOwner);
    SafeHook_SetEnabled(&HT_Recurse, kPreOnlyOwner, true);

    ResetChain();
    HT_State rin = in;
    rin.rcx = 20;
    HT_State out = Run(&HT_Recurse, rin);

    bool pairs = true;
    for (int i = 0; i < kChain; ++i) {
        pairs &= g_chainPre[i] == g_chainPost[i];
    }

    bool firstOwnersOk = g_chainPre[0] == 13 && g_chainPre[31] == 13; // owners 0-31 still fit at level 12
    bool lastOwnersOk = g_chainPre[32] == 12 && g_chainPre[39] == 12;
    bool ok = pairs && g_chainScratchOk && out.rax == 20 && firstOwnersOk && lastOwnersOk && g_chainPreOnly == 21;
    if (!ok) {
        printf("  pairs %s, scratch %s, rax %llu, pre[0] %d pre[32] %d pre-only %d\n", pairs ? "ok" : "BROKEN",
               g_chainScratchOk ? "ok" : "mixed", out.rax, g_chainPre[0], g_chainPre[32], g_chainPreOnly);
    }
    Expect("pool full: no pre without its post, levels kept apart", ok, 0);

    ResetChain();
    rin.rcx = 2;
    Run(&HT_Recurse, rin);
    Expect("after that, every hook runs again", g_chainPre[0] == 3 && g_chainPost[39] == 3 && g_chainPreOnly == 3, 0);

    SafeHook_Remove(&HT_Recurse);
}

} // namespace

int main()
{
    LogOpen(L"hooktest.log");
    if (MH_Initialize() != MH_OK) {
        printf("MH_Initialize failed\n");
        return 1;
    }

    const HT_State in = MakeInput();
    TestBaseline(in);
    TestSingleHooks(in);
    TestCrashContainment(in);
    TestChains(in);
    TestMatchedPairs(in);
    TestHelpers(in);
    TestLongChain(in);
    TestPoolFull(in);

    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
