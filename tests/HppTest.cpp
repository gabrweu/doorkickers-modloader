// include/dk2ml.hpp against a fake API: binding resolution, required vs optional names, typed field/global/function
// access, hooks, events, interfaces, tasks, the GUI wrappers, typed arguments and results.
#include "dk2ml.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void Expect(const char* test, bool ok)
{
    printf("%-58s %s\n", test, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

// --- a fake game ---
struct FakeCamera {
    float pad[3];
    float pos[3];
    int fov;
};

FakeCamera g_camera = {{0, 0, 0}, {1, 2, 3}, 60};
FakeCamera* g_pCamera = &g_camera; // the game's global: a pointer variable

int Twice(int x)
{
    return 2 * x;
}

char g_formatted[64];

int Format(const char* fmt, ...) // a printf-like game function (ImGui::Text, say)
{
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(g_formatted, sizeof(g_formatted), fmt, args);
    va_end(args);
    return n;
}

bool g_newFunctionExists = false;
std::string g_log;

void* FakeResolve(const char* name)
{
    if (!strcmp(name, "Game::Twice")) {
        return reinterpret_cast<void*>(&Twice);
    }
    if (!strcmp(name, "Game::Format")) {
        return reinterpret_cast<void*>(&Format);
    }
    if (!strcmp(name, "g_pCamera")) {
        return &g_pCamera;
    }
    if (!strcmp(name, "Game::NewFunction")) {
        return g_newFunctionExists ? reinterpret_cast<void*>(&Twice) : nullptr;
    }
    return nullptr;
}

int32_t FakeFieldOffset(const char* type, const char* field)
{
    if (!strcmp(type, "Camera") && !strcmp(field, "m_pos")) {
        return static_cast<int32_t>(offsetof(FakeCamera, pos));
    }
    if (!strcmp(type, "Camera") && !strcmp(field, "m_fov")) {
        return static_cast<int32_t>(offsetof(FakeCamera, fov));
    }
    return -1;
}

uint32_t FakeTypeSize(const char* type)
{
    return !strcmp(type, "Camera") ? sizeof(FakeCamera) : 0;
}

DK2ML_Status FakeEnum(const char* type, const char* name, int64_t* out)
{
    if (!strcmp(type, "GUI::eAction") && !strcmp(name, "ACTION_ADD_CHILD")) {
        *out = 12;
        return DK2ML_OK;
    }
    return DK2ML_ERROR;
}

void FakeLog(const char* fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    g_log += buf;
    g_log += '\n';
}

int g_subscribed = 0;
DK2ML_EventType g_subscribedType = static_cast<DK2ML_EventType>(0);

DK2ML_Status FakeSubscribe(DK2ML_EventType type, DK2ML_EventFn fn, void*)
{
    ++g_subscribed;
    g_subscribedType = type;
    return fn ? DK2ML_OK : DK2ML_ERROR;
}

std::string g_published;
const void* g_table = nullptr;

DK2ML_Status FakePublish(const char* name, uint32_t version, const void* table)
{
    g_published = std::string(name) + " v" + std::to_string(version);
    g_table = table;
    return DK2ML_OK;
}

const void* FakeGetInterface(const char* name, uint32_t minVersion, uint32_t* version)
{
    bool found = g_table && g_published.rfind(std::string(name) + " v", 0) == 0 && minVersion <= 2;

    if (version) {
        *version = found ? 2 : 0;
    }
    return found ? g_table : nullptr;
}

int g_tasks = 0;

DK2ML_Status FakeAddTask(DK2ML_TaskFn fn, void*)
{
    ++g_tasks;
    return fn ? DK2ML_OK : DK2ML_ERROR;
}

int g_created = 0;
int g_enabled = 0;

DK2ML_Status FakeCreateSafeHook(void* target, DK2ML_PreFn, DK2ML_PostFn, void*)
{
    ++g_created;
    return target ? DK2ML_OK : DK2ML_ERROR;
}

DK2ML_Status FakeEnable(void*)
{
    ++g_enabled;
    return DK2ML_OK;
}

// GUI kit: one root with one named child
int g_guiRoot = 0;
int g_guiChild = 0;
std::string g_guiText;
int g_callbackEvent = -1;
DK2ML_GuiCallbackFn g_callback = nullptr;
bool g_captured = false;
uint32_t g_guiEvent = 0;

void* FakeGuiFind(void* under, const char* name)
{
    return (!under || under == &g_guiRoot) && !strcmp(name, "#child") ? &g_guiChild : nullptr;
}

DK2ML_Status FakeGuiSetText(void* item, const char* text)
{
    g_guiText = text;
    return item ? DK2ML_OK : DK2ML_ERROR;
}

DK2ML_Status FakeGuiSetCallback(void* item, int itemEvent, DK2ML_GuiCallbackFn fn, void*)
{
    g_callbackEvent = itemEvent;
    g_callback = fn;
    return item ? DK2ML_OK : DK2ML_ERROR;
}

DK2ML_Status FakeCapture(int capture)
{
    g_captured = capture != 0;
    return DK2ML_OK;
}

DK2ML_Status FakeSubscribeGui(uint32_t id, DK2ML_EventFn, void*)
{
    g_guiEvent = id;
    return DK2ML_OK;
}

DK2ML_API MakeApi()
{
    DK2ML_API api = {};
    api.apiVersion = DK2ML_API_VERSION;
    api.structSize = sizeof(DK2ML_API);
    api.ResolveSymbol = FakeResolve;
    api.GetFieldOffset = FakeFieldOffset;
    api.GetTypeSize = FakeTypeSize;
    api.Log = FakeLog;
    api.GetEnumValue = FakeEnum;
    api.CreateSafeHook = FakeCreateSafeHook;
    api.EnableHook = FakeEnable;
    api.Subscribe = FakeSubscribe;
    api.PublishInterface = FakePublish;
    api.GetInterface = FakeGetInterface;
    api.AddTask = FakeAddTask;
    api.GuiFind = FakeGuiFind;
    api.GuiSetText = FakeGuiSetText;
    api.GuiSetCallback = FakeGuiSetCallback;
    api.CaptureGameInput = FakeCapture;
    api.SubscribeGuiEvent = FakeSubscribeGui;
    return api;
}

// --- bindings, as a plugin declares them ---
struct Vec3 {
    float x, y, z;
};

dk2ml::Fn<int(int)> Twice_{"Game::Twice"};
dk2ml::Fn<int(const char*, ...)> Format_{"Game::Format"};
dk2ml::Global<FakeCamera*> Camera_{"g_pCamera"};
dk2ml::Field<Vec3> CameraPos{"Camera", "m_pos"};
dk2ml::Field<int> CameraFov{"Camera", "m_fov"};
dk2ml::TypeSize CameraSize{"Camera"};
dk2ml::Enum AddChild{"GUI::eAction", "ACTION_ADD_CHILD"};
dk2ml::Fn<int(int)> Optional_{"Game::Gone", dk2ml::Optional};
dk2ml::Fn<int(int)> NewFunction{"Game::NewFunction"}; // required, missing until g_newFunctionExists

int Pre(DK2ML_Regs*, void*)
{
    return DK2ML_CALL_ORIGINAL;
}

// --- the sections, in main's order ---

void CheckResolveAll(const DK2ML_API* api)
{
    Expect("a missing required name fails ResolveAll", !dk2ml::ResolveAll(api));
    Expect("...and is logged by name", g_log.find("missing function Game::NewFunction") != std::string::npos);
    Expect("a missing optional name is logged too", g_log.find("missing function Game::Gone") != std::string::npos);
    Expect("...and the summary counts both kinds", g_log.find("1 required and 1 optional") != std::string::npos);

    g_newFunctionExists = true;
    g_log.clear();
    Expect("with only optional names missing, ResolveAll succeeds", dk2ml::ResolveAll(api));
    Expect("optional binding reports it's missing", !Optional_ && !Optional_.Resolved() && NewFunction.Resolved());
}

void CheckBindings(const DK2ML_API* api)
{
    Expect("Fn calls the game function", Twice_(21) == 42);

    bool variadicOk = Format_("%d troopers, %.1f m", 4, 2.5) == 17 && !strcmp(g_formatted, "4 troopers, 2.5 m") &&
                      Format_("plain") == 5;
    Expect("a variadic Fn passes the extra arguments", variadicOk);
    Expect("Global is the variable, *Global its value", Camera_.Address() == &g_pCamera && *Camera_ == &g_camera);

    Vec3& pos = CameraPos(*Camera_);
    Expect("Field reads through the PDB offset", pos.x == 1 && pos.y == 2 && pos.z == 3);
    CameraFov(&g_camera) = 75;
    Expect("Field writes into the object", g_camera.fov == 75);
    Expect("TypeSize and Enum", CameraSize.Get() == sizeof(FakeCamera) && AddChild.Get() == 12);

    Expect("Hook creates and enables", dk2ml::Hook(api, Twice_, Pre) && g_created == 1 && g_enabled == 1);
    bool on = dk2ml::On(api, DK2ML_EVENT_STATE_CHANGED, [](const DK2ML_Event*, void*) {});
    Expect("On subscribes a captureless lambda",
           on && g_subscribed == 1 && g_subscribedType == DK2ML_EVENT_STATE_CHANGED);
}

void CheckInterfacesAndTasks(const DK2ML_API* api)
{
    struct MathTable {
        uint32_t structSize;
        int (*Add)(int, int);
    };

    static const MathTable math = {sizeof(MathTable), [](int a, int b) { return a + b; }};
    uint32_t version = 99;
    Expect("Publish hands the table over", dk2ml::Publish(api, "test.Math", 2, &math) && g_published == "test.Math v2");

    const MathTable* got = dk2ml::Get<MathTable>(api, "test.Math", 1, &version);
    Expect("Get returns the typed table and its version", got == &math && version == 2 && got->Add(2, 3) == 5);
    Expect("Get: a newer minVersion is NULL",
           dk2ml::Get<MathTable>(api, "test.Math", 3, &version) == nullptr && version == 0);

    Expect("Post queues a captureless lambda", dk2ml::Post(api, [](void*) {}) && g_tasks == 1);
}

void CheckGuiWrappers(const DK2ML_API* api)
{
    bool found = dk2ml::gui::Find(api, "#child") == &g_guiChild &&
                 dk2ml::gui::Find(api, "#child", &g_guiRoot) == &g_guiChild &&
                 dk2ml::gui::Find(api, "#none") == nullptr;
    Expect("gui::Find from the GUI root or under an item", found);
    Expect("gui::SetText", dk2ml::gui::SetText(api, &g_guiChild, "Hello") && g_guiText == "Hello");

    bool bound = dk2ml::gui::OnAction(api, &g_guiChild, 2, [](void*, float, float, void*) {});
    Expect("gui::OnAction binds a captureless lambda to an item event", bound && g_callbackEvent == 2 && g_callback);

    bool captureAndEvent = dk2ml::CaptureInput(api, true) && g_captured &&
                           dk2ml::OnGuiEvent(api, 148, [](const DK2ML_Event*, void*) {}) && g_guiEvent == 148;
    Expect("CaptureInput / OnGuiEvent", captureAndEvent);
    Expect("OnGuiEvent refuses an id that isn't one (e.g. an unresolved Enum)",
           !dk2ml::OnGuiEvent(api, 0, [](const DK2ML_Event*, void*) {}) && g_guiEvent == 148);
}

void CheckTypedArguments()
{
    uint64_t stack[8] = {};
    DK2ML_Regs r = {};
    r.stack = stack;
    r.rcx = 0xDEADBEEF00000001ull; // a bool in cl: the upper bytes are garbage, as the convention allows
    r.rdx = 0xFFFFFFFF0000FFFFull; // int -> only edx counts

    float f = 2.5f;
    uint32_t fbits;
    memcpy(&fbits, &f, 4);
    r.xmm[2].lo = 0xA5A5A5A500000000ull | fbits; // float argument 2

    double d = 0.125;
    memcpy(&stack[5], &d, 8); // double argument 4, on the stack
    stack[6] = reinterpret_cast<uint64_t>(&g_camera); // pointer argument 5

    Expect("Arg<bool> ignores the garbage upper bytes", dk2ml::Arg<bool>(&r, 0) == true);
    Expect("Arg<int> takes the low 32 bits", dk2ml::Arg<int>(&r, 1) == 0x0000FFFF);
    Expect("Arg<float> from xmm, Arg<double> from the stack",
           dk2ml::Arg<float>(&r, 2) == 2.5f && dk2ml::Arg<double>(&r, 4) == 0.125);
    Expect("Arg<T*> from the stack", dk2ml::Arg<FakeCamera*>(&r, 5) == &g_camera);

    dk2ml::SetArg(&r, 2, 4.0f);
    Expect("SetArg<float> keeps the register's upper bits",
           dk2ml::Arg<float>(&r, 2) == 4.0f && (r.xmm[2].lo >> 32) == 0xA5A5A5A5u);
    dk2ml::SetArg(&r, 1, 7);
    Expect("SetArg<int>", dk2ml::Arg<int>(&r, 1) == 7);

    dk2ml::SetResult(&r, -5);
    dk2ml::SetResult(&r, 1.5f);
    Expect("SetResult/Result for int (rax) and float (xmm0)",
           dk2ml::Result<int>(&r) == -5 && dk2ml::Result<float>(&r) == 1.5f);
}

} // namespace

int main()
{
    const DK2ML_API api = MakeApi();

    CheckResolveAll(&api);

    CheckBindings(&api);

    CheckInterfacesAndTasks(&api);

    CheckGuiWrappers(&api);

    CheckTypedArguments();

    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
