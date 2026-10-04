// Checks the GUI callback thunks and input capture (loader/gui/GuiThunks.cpp) without the game. A fake GUI::sAction is
// run by calling its pCallback the way the game does (void(const sAction&)). The plugin's function must get the
// action's owner, the cursor and its user pointer, under crash containment.
// usage: guitest.exe   (prints "all passed", exit code 0)
#include "../loader/Loader.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void Expect(const char* test, bool ok)
{
    printf("%-62s %s\n", test, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

const HMODULE kA = reinterpret_cast<HMODULE>(0x10000);
const HMODULE kB = reinterpret_cast<HMODULE>(0x20000);

// the game's GUI::sAction, build 112: owner +0, eventParams +48 (owner +0, cursor +8 within it), pCallback +72
struct FakeAction {
    void* owner;
    char pad[40];
    void* eventOwner;
    float cursor[2];
    int iParams[2];
    void (*pCallback)(const void* action);
};

static_assert(offsetof(FakeAction, eventOwner) == 48 && offsetof(FakeAction, pCallback) == 72, "sAction layout");

// what the callbacks saw
std::string g_calls;
void* g_item = nullptr;
float g_x = 0;
float g_y = 0;

// what the crash callback saw
HMODULE g_crashed = nullptr;
int g_crashes = 0;

void Callback(void* item, float x, float y, void* user)
{
    g_calls += static_cast<const char*>(user);
    g_item = item;
    g_x = x;
    g_y = y;
}

void Crash(void*, float, float, void*)
{
    g_calls += "x";
    *static_cast<volatile int*>(nullptr) = 1;
}

void OnCrash(HMODULE owner)
{
    g_crashed = owner;
    ++g_crashes;
}

// what the game does when the action runs
void Run(FakeAction& a)
{
    a.pCallback(&a);
}

std::string Calls()
{
    std::string s = g_calls;
    g_calls.clear();
    return s;
}

using ActionFn = void (*)(const void*);

// plugin A's two thunks: user pointer "a" and "b"
struct Thunks {
    void* a;
    void* b;
};

Thunks MakeThunks()
{
    void* thunk = Gui_CallbackThunk(kA, Callback, (void*)"a");
    Expect("a thunk is made", thunk != nullptr && Gui_IsThunk(thunk));
    Expect("the same callback again shares it", Gui_CallbackThunk(kA, Callback, (void*)"a") == thunk);

    void* other = Gui_CallbackThunk(kA, Callback, (void*)"b");
    Expect("another user pointer gets its own", other && other != thunk && Gui_CallbackCount(kA) == 2);
    Expect("NULL fn is refused", Gui_CallbackThunk(kA, nullptr, nullptr) == nullptr);

    return {thunk, other};
}

// Leaves `a` on thunk "b".
void CheckCallbacksRun(FakeAction& a, const Thunks& thunks, int* itemA, int* itemClone)
{
    a.owner = itemA;
    a.cursor[0] = 12.5f;
    a.cursor[1] = -40.f;
    a.pCallback = reinterpret_cast<ActionFn>(thunks.a);
    Run(a);
    Expect("the callback gets the action's owner, the cursor and user",
           Calls() == "a" && g_item == itemA && g_x == 12.5f && g_y == -40.f);

    FakeAction clone = a; // cloning copies pCallback and sets owner to the clone
    clone.owner = itemClone;
    Run(clone);
    Expect("a clone's action reaches it with the clone", Calls() == "a" && g_item == itemClone);

    a.pCallback = reinterpret_cast<ActionFn>(thunks.b);
    Run(a);
    Expect("each thunk keeps its own user pointer", Calls() == "b");
}

void CheckCrashContained(FakeAction& a, int* itemA)
{
    FakeAction bad = {};
    bad.owner = itemA;
    bad.pCallback = reinterpret_cast<ActionFn>(Gui_CallbackThunk(kB, Crash, nullptr));
    Expect("a capture is taken", Gui_SetCapture(kB, true) == DK2ML_OK && Gui_AnyCapture() && Gui_Captures(kB));

    Run(bad);
    Expect("a crashing callback is contained and its plugin switched off once",
           Calls() == "x" && g_crashes == 1 && g_crashed == kB);

    Run(bad);
    Expect("its thunk does nothing from then on", Calls().empty() && g_crashes == 1);
    Expect("its capture was released, and it can't capture again",
           !Gui_AnyCapture() && Gui_SetCapture(kB, true) == DK2ML_ERROR);

    Run(a);
    Expect("other plugins' callbacks still run", Calls() == "b");
}

void CheckCaptureAndFault(FakeAction& a)
{
    Expect("capturing twice is one capture",
           Gui_SetCapture(kA, true) == DK2ML_OK && Gui_SetCapture(kA, true) == DK2ML_OK && Gui_AnyCapture());
    Gui_SetCapture(kA, false);
    Expect("released with one 0", !Gui_AnyCapture() && !Gui_Captures(kA));

    Gui_FaultOwner(kA);
    Run(a);
    Expect("a plugin switched off elsewhere gets no callbacks", Calls().empty());

    bool refused =
        !Gui_CallbackThunk(kA, Callback, reinterpret_cast<void*>(0x77)) && !Gui_CallbackThunk(kB, Callback, nullptr);
    Expect("and can't set new ones", refused);
}

void CheckThunkCap()
{
    int made = 0;
    HMODULE kMany = reinterpret_cast<HMODULE>(0x30000);
    while (Gui_CallbackThunk(kMany, Callback, reinterpret_cast<void*>(static_cast<uintptr_t>(made + 1))) &&
           made < 10000) {
        ++made;
    }
    Expect("thunks are capped (4096 in all)", made == 4096 - 3);
}

} // namespace

int main()
{
    LogOpen(L"guitest.log");
    Gui_SetCrashCallback(OnCrash);
    Gui_SetCallbackLayout({0, 48 + 8});

    int itemA = 0;
    int itemClone = 0;
    Thunks thunks = MakeThunks();

    FakeAction a = {};
    CheckCallbacksRun(a, thunks, &itemA, &itemClone);
    CheckCrashContained(a, &itemA);
    CheckCaptureAndFault(a);
    CheckThunkCap();

    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
