// Checks the plugins' events (loader/plugins/Events.cpp) without the game: subscriptions only during init, order, crash
// containment per subscriber, STATE_CHANGED only on a change, GUI events by id, tasks (AddTask). Also checks the
// interfaces (loader/plugins/Interfaces.cpp).
// usage: eventstest.exe   (prints "all passed", exit code 0)
#include "../loader/Loader.h"

#include <cstdio>
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

const HMODULE kA = reinterpret_cast<HMODULE>(0x10000);
const HMODULE kB = reinterpret_cast<HMODULE>(0x20000);
const HMODULE kC = reinterpret_cast<HMODULE>(0x30000);
const HMODULE kD = reinterpret_cast<HMODULE>(0x40000);
const HMODULE kE = reinterpret_cast<HMODULE>(0x50000);
const HMODULE kF = reinterpret_cast<HMODULE>(0x60000);

// what the callbacks saw
std::string g_calls;
HMODULE g_crashed = nullptr;
int g_crashes = 0;
int64_t g_old = 0;
int64_t g_new = 0;
void* g_client = nullptr;
bool g_sizeOk = true;

void FrameA(const DK2ML_Event* e, void* user)
{
    g_calls += static_cast<const char*>(user);
    g_client = e->gameClient;
    g_sizeOk &= e->structSize == sizeof(DK2ML_Event) && e->type == DK2ML_EVENT_FRAME;
}

void FrameCrash(const DK2ML_Event*, void*)
{
    g_calls += "x";
    *static_cast<volatile int*>(nullptr) = 1;
}

void State(const DK2ML_Event* e, void*)
{
    g_calls += "s";
    g_old = e->oldState;
    g_new = e->newState;
}

void Loaded(const DK2ML_Event* e, void*)
{
    g_calls += "L";
    g_sizeOk &= e->type == DK2ML_EVENT_PLUGINS_LOADED && e->gameClient == nullptr;
}

uint32_t g_guiId = 0;
const void* g_guiParams = nullptr;
int32_t g_width = 0;
int32_t g_height = 0;

void GuiEvent(const DK2ML_Event* e, void* user)
{
    g_calls += static_cast<const char*>(user);
    g_guiId = e->guiEventId;
    g_guiParams = e->guiEventParams;
    g_sizeOk &= e->type == DK2ML_EVENT_GUI_EVENT;
}

void GuiEventCrash(const DK2ML_Event*, void*)
{
    g_calls += "G";
    *static_cast<volatile int*>(nullptr) = 1;
}

void Resized(const DK2ML_Event* e, void*)
{
    g_calls += "r";
    g_width = e->width;
    g_height = e->height;
    g_sizeOk &= e->type == DK2ML_EVENT_WINDOW_RESIZED;
}

void Task(void* user)
{
    g_calls += static_cast<const char*>(user);
}

void TaskCrash(void*)
{
    g_calls += "X";
    *static_cast<volatile int*>(nullptr) = 1;
}

void TaskQueuesAnother(void*)
{
    g_calls += "q";
    Events_AddTask(kA, Task, (void*)"n");
}

struct Table {
    uint32_t structSize;
    int (*Add)(int, int);
};

const Table g_table = {sizeof(Table), [](int a, int b) { return a + b; }};
const Table g_tableB = {sizeof(Table), [](int a, int b) { return a - b; }};

void OnCrash(HMODULE owner)
{
    g_crashed = owner;
    ++g_crashes;
}

std::string Calls()
{
    std::string s = g_calls;
    g_calls.clear();
    return s;
}

// subscriptions: only while plugins initialize; leaves Events open for the interfaces section
void TestSubscriptions()
{
    Expect("Subscribe before plugins initialize is refused",
           Events_Subscribe(kA, DK2ML_EVENT_FRAME, FrameA, (void*)"a") == DK2ML_ERROR);

    Events_SetOpen(true);
    bool ok = Events_Subscribe(kA, DK2ML_EVENT_FRAME, FrameA, (void*)"a") == DK2ML_OK &&
              Events_Subscribe(kB, DK2ML_EVENT_FRAME, FrameCrash, nullptr) == DK2ML_OK &&
              Events_Subscribe(kC, DK2ML_EVENT_FRAME, FrameA, (void*)"c") == DK2ML_OK &&
              Events_Subscribe(kA, DK2ML_EVENT_STATE_CHANGED, State, nullptr) == DK2ML_OK;
    Expect("subscriptions during init", ok);

    Expect("unknown type and NULL callback are refused",
           Events_Subscribe(kA, static_cast<DK2ML_EventType>(99), FrameA, nullptr) == DK2ML_ERROR &&
               Events_Subscribe(kA, DK2ML_EVENT_FRAME, nullptr, nullptr) == DK2ML_ERROR);
    Expect("PLUGINS_LOADED can be subscribed to",
           Events_Subscribe(kA, DK2ML_EVENT_PLUGINS_LOADED, Loaded, nullptr) == DK2ML_OK);

    bool guiOk = Events_SubscribeGui(kA, 148, GuiEvent, (void*)"g") == DK2ML_OK &&
                 Events_SubscribeGui(kD, 149, GuiEvent, (void*)"h") == DK2ML_OK &&
                 Events_SubscribeGui(kF, 149, GuiEventCrash, nullptr) == DK2ML_OK &&
                 Events_SubscribeGui(kD, 148, GuiEvent, (void*)"d") == DK2ML_OK;
    Expect("GUI events are subscribed by id", guiOk);

    bool guiRefused = Events_SubscribeGui(kA, 0, GuiEvent, nullptr) == DK2ML_ERROR &&
                      Events_SubscribeGui(kA, 148, nullptr, nullptr) == DK2ML_ERROR &&
                      Events_Subscribe(kA, DK2ML_EVENT_GUI_EVENT, GuiEvent, nullptr) == DK2ML_ERROR;
    Expect("GUI event id 0, a NULL callback, and GUI_EVENT through Subscribe are refused", guiRefused);

    Expect("WINDOW_RESIZED can be subscribed to",
           Events_Subscribe(kA, DK2ML_EVENT_WINDOW_RESIZED, Resized, nullptr) == DK2ML_OK);
}

// interfaces: published while plugins initialize, looked up only after
void TestInterfaces()
{
    Expect("PublishInterface before init is refused", Interfaces_Publish(kD, "test.Math", 1, &g_table) == DK2ML_ERROR);

    Interfaces_SetOpen(true);
    Expect("PublishInterface during init", Interfaces_Publish(kD, "test.Math", 2, &g_table) == DK2ML_OK &&
                                               Interfaces_Publish(kE, "test.Other", 1, &g_tableB) == DK2ML_OK);
    Expect("a name taken by another plugin is refused",
           Interfaces_Publish(kE, "test.Math", 1, &g_tableB) == DK2ML_ERROR);

    bool badRefused = Interfaces_Publish(kE, "", 1, &g_table) == DK2ML_ERROR &&
                      Interfaces_Publish(kE, nullptr, 1, &g_table) == DK2ML_ERROR &&
                      Interfaces_Publish(kE, "has space", 1, &g_table) == DK2ML_ERROR &&
                      Interfaces_Publish(kE, "0123456789012345678901234567890123456789012345678901234567890123", 1,
                                         &g_table) == DK2ML_ERROR &&
                      Interfaces_Publish(kE, "test.Null", 1, nullptr) == DK2ML_ERROR;
    Expect("bad names and a NULL table are refused", badRefused);

    uint32_t version = 99;
    Expect("GetInterface during init is NULL (load order mustn't matter)",
           Interfaces_Get(kA, "test.Math", 0, &version) == nullptr && version == 0);

    Interfaces_SetOpen(false);
    Expect("PublishInterface after init is refused", Interfaces_Publish(kE, "test.Late", 1, &g_table) == DK2ML_ERROR);
    Expect("GetInterface after init finds it, with its version",
           Interfaces_Get(kA, "test.Math", 2, &version) == &g_table && version == 2 &&
               static_cast<const Table*>(Interfaces_Get(kA, "test.Math", 0, nullptr))->Add(2, 3) == 5);
    Expect("a newer minVersion than published is NULL",
           Interfaces_Get(kA, "test.Math", 3, &version) == nullptr && version == 0);
    Expect("an unknown name is NULL", Interfaces_Get(kA, "test.Nope", 0, nullptr) == nullptr);

    std::vector<std::string> published;
    std::vector<std::string> used;
    Interfaces_Of(kA, &published, &used);
    Expect("the consumer is recorded as using it", published.empty() && used.size() == 1 && used[0] == "test.Math");

    published.clear();
    used.clear();
    Interfaces_Of(kD, &published, &used);
    Expect("the publisher is recorded as offering it", published.size() == 1 && used.empty());

    Interfaces_FaultOwner(kD);
    Expect("a switched-off publisher's table isn't handed out",
           Interfaces_Get(kA, "test.Math", 0, nullptr) == nullptr &&
               Interfaces_Get(kA, "test.Other", 0, nullptr) == &g_tableB);
}

// after init: PLUGINS_LOADED, then frames with STATE_CHANGED and crash containment
void TestFrames(void* client)
{
    Events_SetOpen(false);
    Events_Dispatch(DK2ML_EVENT_PLUGINS_LOADED, nullptr, -1, -1);
    Expect("PLUGINS_LOADED reaches its subscriber, without a GameClient", Calls() == "L" && g_sizeOk);
    Expect("Subscribe after init is refused",
           Events_Subscribe(kA, DK2ML_EVENT_FRAME, FrameA, (void*)"a") == DK2ML_ERROR);
    Expect("Wanted: FRAME yes, MAP_LOADED no",
           Events_Wanted(DK2ML_EVENT_FRAME) && !Events_Wanted(DK2ML_EVENT_MAP_LOADED));

    Events_Frame(client, 3);
    std::string calls = Calls();
    Expect("first frame: STATE_CHANGED -1 -> 3, then FRAME in order", calls == "saxc" && g_old == -1 && g_new == 3);
    Expect("a crashing subscriber's plugin is switched off once", g_crashes == 1 && g_crashed == kB);
    Expect("the event carries its size, type and the GameClient", g_sizeOk && g_client == client);

    Events_Frame(client, 3);
    Expect("same state: no STATE_CHANGED; the crashed one stays out", Calls() == "ac");
    Events_Frame(client, 8);
    Expect("state 3 -> 8: STATE_CHANGED again", Calls() == "sac" && g_old == 3 && g_new == 8);

    Events_FaultOwner(kC); // e.g. C crashed in a hook
    Events_Frame(client, 8);
    Expect("a plugin switched off elsewhere gets no more events", Calls() == "a" && g_crashes == 1);
    Events_Dispatch(DK2ML_EVENT_MAP_LOADED, client, 8, 8);
    Expect("an event nobody wants is a no-op", Calls().empty());
}

// GUI events: by id, every subscriber of that id in order, crash-contained
void TestGuiEvents(void* client)
{
    Expect("Subscribe/SubscribeGui after init are refused",
           Events_SubscribeGui(kA, 150, GuiEvent, (void*)"z") == DK2ML_ERROR);

    std::vector<uint32_t> ids = Events_GuiEventIds();
    Expect("the wanted GUI event ids, each once",
           ids.size() == 2 && ids[0] == 148 && ids[1] == 149 && Events_Wanted(DK2ML_EVENT_GUI_EVENT));

    int params = 0;
    Events_DispatchGui(148, &params, client, 8);
    Expect("a GUI event reaches its id's subscribers in order, with id and params",
           Calls() == "gd" && g_guiId == 148 && g_guiParams == &params && g_sizeOk);
    Events_DispatchGui(150, &params, client, 8);
    Expect("a GUI event nobody subscribed to is a no-op", Calls().empty());

    int crashesBeforeGui = g_crashes;
    Events_DispatchGui(149, nullptr, client, 8);
    Expect("a crashing GUI event subscriber is switched off; the others run",
           Calls() == "hG" && g_crashes == crashesBeforeGui + 1 && g_crashed == kF);
    Events_DispatchGui(149, nullptr, client, 8);
    Expect("and gets no more GUI events", Calls() == "h");

    std::vector<std::string> types = Events_TypesOf(kD);
    Expect("subscriptions name the GUI event ids", types.size() == 1 && types[0] == "GUI_EVENT 148,149");

    Events_DispatchResize(client, 8, 1920, 1080);
    Expect("WINDOW_RESIZED carries the size", Calls() == "r" && g_width == 1920 && g_height == 1080 && g_sizeOk);
}

// tasks: queued from anywhere, run first thing in the next frame, in order, under crash containment
void TestTasks(void* client)
{
    Expect("AddTask(NULL) is refused", Events_AddTask(kA, nullptr, nullptr) == DK2ML_ERROR);

    bool queuedAll =
        Events_AddTask(kA, Task, (void*)"1") == DK2ML_OK && Events_AddTask(kD, Task, (void*)"2") == DK2ML_OK &&
        Events_AddTask(kA, TaskQueuesAnother, nullptr) == DK2ML_OK && Events_AddTask(kA, Task, (void*)"3") == DK2ML_OK;
    Expect("AddTask queues", queuedAll);
    Expect("tasks don't run before a frame", Calls().empty());

    Events_Frame(client, 8);
    Expect("tasks run in order before the frame's events", Calls() == "12q3a");
    Events_Frame(client, 8);
    Expect("a task queued by a task waits for the next frame", Calls() == "na");
    Expect("a switched-off plugin can't queue tasks", Events_AddTask(kC, Task, (void*)"c") == DK2ML_ERROR);

    int crashesBefore = g_crashes;
    Events_AddTask(kE, TaskCrash, nullptr);
    Events_AddTask(kE, Task, (void*)"e"); // after the crash: switched off, never runs
    Events_AddTask(kA, Task, (void*)"4");
    Events_Frame(client, 8);
    Expect("a crashing task switches its plugin off once; others run",
           Calls() == "X4a" && g_crashes == crashesBefore + 1 && g_crashed == kE);
    Expect("its later AddTask calls are refused", Events_AddTask(kE, Task, (void*)"e") == DK2ML_ERROR);

    int queued = 0;
    while (Events_AddTask(kA, Task, (void*)"") == DK2ML_OK && queued < 10000) {
        ++queued;
    }
    Expect("the queue is capped at 4096", queued == 4096);
    Events_Frame(client, 8);
    Expect("after the frame there is room again", Events_AddTask(kA, Task, (void*)"5") == DK2ML_OK);

    Events_FaultOwner(kA);
    Events_Frame(client, 8);
    Expect("waiting tasks of a plugin switched off are dropped", Calls().find('5') == std::string::npos);

    Events_SetTasksAvailable(false);
    Expect("no frame tick in this game build: AddTask is refused", Events_AddTask(kD, Task, (void*)"d") == DK2ML_ERROR);
}

} // namespace

int main()
{
    LogOpen(L"eventstest.log");
    Events_SetCrashCallback(OnCrash);
    int dummy = 0;
    void* client = &dummy;

    TestSubscriptions();
    TestInterfaces();
    TestFrames(client);
    TestGuiEvents(client);
    TestTasks(client);

    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
