// Events (DK2ML_API::Subscribe): each source is hooked once (GameHooks.cpp); every subscriber gets called.
//
// Subscriptions are taken only during init, so dispatch reads the lists without a lock. Callbacks run under __try; a
// crash switches off that plugin. Tasks (AddTask) come from any thread and wait in a locked queue for the next frame.
// Doesn't touch the game (eventstest).
#include "Loader.h"

#include <malloc.h> // _resetstkoflw

#include <algorithm>
#include <cstdio>

namespace {

struct Subscriber {
    DK2ML_EventFn fn;
    void* user;
    HMODULE owner;
    volatile bool faulted; // owner switched off
};

constexpr int kEventTypes = DK2ML_EVENT_WINDOW_RESIZED + 1;
std::vector<Subscriber> g_subscribers[kEventTypes]; // GUI_EVENT's stays empty (by id, below)

// SubscribeGuiEvent, by GUI::Events::eEventType id
struct GuiSubscriber {
    uint32_t id;
    Subscriber s;
};

std::vector<GuiSubscriber> g_guiSubscribers;
constexpr uint32_t kMaxGuiEventId = 0xFFFF; // the game has 259; GuiKit.cpp checks the real range

bool g_open = false;
int64_t g_lastState = -1; // -1: no GameClient
void (*g_onCrash)(HMODULE owner) = nullptr;

struct Task {
    DK2ML_TaskFn fn;
    void* user;
    HMODULE owner;
};

// Capped: frames stop during random map generation.
constexpr size_t kMaxTasks = 4096;
SRWLOCK g_tasksLock = SRWLOCK_INIT; // the queue and the lists below
std::vector<Task> g_tasks;
std::vector<HMODULE> g_faultedOwners; // their tasks are dropped
std::vector<HMODULE> g_toldFull; // full-queue refusal already logged
bool g_tasksAvailable = true; // false: no frame tick in this build

thread_local DWORD t_code;

int Filter(DWORD code)
{
    t_code = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

// No destructors in here (__try).
bool CallSubscriber(const Subscriber& s, const DK2ML_Event* event)
{
    __try {
        s.fn(event, s.user);
        return true;
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

bool CallTask(const Task& t)
{
    __try {
        t.fn(t.user);
        return true;
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

bool IsFaulted(HMODULE owner) // under g_tasksLock
{
    for (HMODULE m : g_faultedOwners) {
        if (m == owner) {
            return true;
        }
    }
    return false;
}

// True the first time per plugin, and records it.
bool FirstFullQueueRefusal(HMODULE owner) // under g_tasksLock
{
    bool told = false;
    for (HMODULE m : g_toldFull) {
        told |= m == owner;
    }
    if (!told) {
        g_toldFull.push_back(owner);
    }
    return !told;
}

void Contain(HMODULE owner, const char* where)
{
    if (t_code == EXCEPTION_STACK_OVERFLOW) {
        _resetstkoflw();
    }

    LogF("%ls crashed in its %s (exception 0x%08lX)", Log_ModuleName(owner).c_str(), where, t_code);
    Events_FaultOwner(owner);
    if (g_onCrash) {
        g_onCrash(owner); // its hooks and options too
    }
}

const char* TypeName(DK2ML_EventType type)
{
    switch (type) {
    case DK2ML_EVENT_FRAME: return "FRAME";
    case DK2ML_EVENT_GUI_LOADED: return "GUI_LOADED";
    case DK2ML_EVENT_STATE_CHANGED: return "STATE_CHANGED";
    case DK2ML_EVENT_MAP_LOADED: return "MAP_LOADED";
    case DK2ML_EVENT_PLUGINS_LOADED: return "PLUGINS_LOADED";
    case DK2ML_EVENT_GUI_EVENT: return "GUI_EVENT";
    case DK2ML_EVENT_WINDOW_RESIZED: return "WINDOW_RESIZED";
    }
    return "?";
}

void Deliver(const Subscriber& s, const DK2ML_Event& event)
{
    if (s.faulted) {
        return;
    }

    if (!CallSubscriber(s, &event)) {
        char where[64];
        snprintf(where, sizeof(where), "%s event callback", TypeName(event.type));
        Contain(s.owner, where);
    }
}

DK2ML_Event MakeEvent(DK2ML_EventType type, void* gameClient, int64_t oldState, int64_t newState)
{
    DK2ML_Event event = {};
    event.structSize = sizeof(DK2ML_Event);
    event.type = type;
    event.gameClient = gameClient;
    event.oldState = oldState;
    event.newState = newState;
    return event;
}

} // namespace

void Events_SetOpen(bool open)
{
    g_open = open;
}

void Events_SetCrashCallback(void (*onCrash)(HMODULE owner))
{
    g_onCrash = onCrash;
}

const char* Events_Check(int type, DK2ML_EventFn fn)
{
    if (type < DK2ML_EVENT_FRAME || type >= kEventTypes) {
        return "unknown event type";
    }
    if (type == DK2ML_EVENT_GUI_EVENT) {
        return "GUI events are subscribed by id: use SubscribeGuiEvent";
    }
    if (!fn) {
        return "no callback";
    }
    return nullptr;
}

const char* Events_CheckGui(uint32_t id, DK2ML_EventFn fn)
{
    if (id == 0 || id > kMaxGuiEventId) {
        return "not a GUI event id";
    }
    if (!fn) {
        return "no callback";
    }
    return nullptr;
}

DK2ML_Status Events_SubscribeGui(HMODULE owner, uint32_t id, DK2ML_EventFn fn, void* user)
{
    std::wstring who = Log_ModuleName(owner);
    if (!g_open) {
        LogF("%ls: SubscribeGuiEvent after DK2ML_PluginInit returned, ignored (subscribe in init)", who.c_str());
        return DK2ML_ERROR;
    }
    if (const char* error = Events_CheckGui(id, fn)) {
        LogF("%ls: SubscribeGuiEvent(%u) ignored: %s", who.c_str(), id, error);
        return DK2ML_ERROR;
    }

    g_guiSubscribers.push_back({id, {fn, user, owner, false}});
    return DK2ML_OK;
}

std::vector<uint32_t> Events_GuiEventIds()
{
    std::vector<uint32_t> ids;
    for (const auto& g : g_guiSubscribers) {
        if (std::find(ids.begin(), ids.end(), g.id) == ids.end()) {
            ids.push_back(g.id);
        }
    }

    std::sort(ids.begin(), ids.end());
    return ids;
}

void Events_DispatchGui(uint32_t id, const void* params, void* gameClient, int64_t state)
{
    DK2ML_Event event = MakeEvent(DK2ML_EVENT_GUI_EVENT, gameClient, state, state);
    event.guiEventId = id;
    event.guiEventParams = params;

    for (const auto& g : g_guiSubscribers) {
        if (g.id == id) {
            Deliver(g.s, event);
        }
    }
}

void Events_DispatchResize(void* gameClient, int64_t state, int32_t width, int32_t height)
{
    if (!Events_Wanted(DK2ML_EVENT_WINDOW_RESIZED)) {
        return;
    }

    DK2ML_Event event = MakeEvent(DK2ML_EVENT_WINDOW_RESIZED, gameClient, state, state);
    event.width = width;
    event.height = height;

    for (const Subscriber& s : g_subscribers[DK2ML_EVENT_WINDOW_RESIZED]) {
        Deliver(s, event);
    }
}

DK2ML_Status Events_Subscribe(HMODULE owner, int type, DK2ML_EventFn fn, void* user)
{
    std::wstring who = Log_ModuleName(owner);
    if (!g_open) {
        LogF("%ls: Subscribe after DK2ML_PluginInit returned, ignored (subscribe in init)", who.c_str());
        return DK2ML_ERROR;
    }
    if (const char* error = Events_Check(type, fn)) {
        LogF("%ls: Subscribe(%d) ignored: %s", who.c_str(), type, error);
        return DK2ML_ERROR;
    }

    g_subscribers[type].push_back({fn, user, owner, false});
    return DK2ML_OK;
}

bool Events_Wanted(DK2ML_EventType type)
{
    if (type == DK2ML_EVENT_GUI_EVENT) {
        return !g_guiSubscribers.empty();
    }
    return type >= DK2ML_EVENT_FRAME && type < kEventTypes && !g_subscribers[type].empty();
}

void Events_FaultOwner(HMODULE owner)
{
    for (auto& list : g_subscribers) {
        for (auto& s : list) {
            if (s.owner == owner) {
                s.faulted = true;
            }
        }
    }
    for (auto& g : g_guiSubscribers) {
        if (g.s.owner == owner) {
            g.s.faulted = true;
        }
    }

    auto isTheirs = [owner](const Task& t) { return t.owner == owner; };
    AcquireSRWLockExclusive(&g_tasksLock);
    if (!IsFaulted(owner)) {
        g_faultedOwners.push_back(owner);
    }
    g_tasks.erase(std::remove_if(g_tasks.begin(), g_tasks.end(), isTheirs), g_tasks.end());
    ReleaseSRWLockExclusive(&g_tasksLock);
}

void Events_Dispatch(DK2ML_EventType type, void* gameClient, int64_t oldState, int64_t newState)
{
    if (type == DK2ML_EVENT_GUI_EVENT || !Events_Wanted(type)) {
        return; // GUI events go through Events_DispatchGui
    }

    DK2ML_Event event = MakeEvent(type, gameClient, oldState, newState);
    for (const Subscriber& s : g_subscribers[type]) {
        Deliver(s, event);
    }
}

void Events_SetTasksAvailable(bool available)
{
    AcquireSRWLockExclusive(&g_tasksLock);
    g_tasksAvailable = available;
    size_t dropped = available ? 0 : g_tasks.size();
    if (!available) {
        g_tasks.clear();
    }
    ReleaseSRWLockExclusive(&g_tasksLock);

    if (dropped) {
        LogF("%zu task(s) dropped: this game build has no frame tick to run them", dropped);
    }
}

DK2ML_Status Events_AddTask(HMODULE owner, DK2ML_TaskFn fn, void* user)
{
    if (!fn) {
        return DK2ML_ERROR;
    }

    const char* refused = nullptr; // "": refused without a log line
    AcquireSRWLockExclusive(&g_tasksLock);
    if (!g_tasksAvailable) {
        refused = "this game build has no frame tick";
    } else if (IsFaulted(owner)) {
        refused = ""; // switched off, already logged
    } else if (g_tasks.size() >= kMaxTasks) {
        refused = FirstFullQueueRefusal(owner) ? "too many tasks waiting (4096)" : "";
    } else {
        g_tasks.push_back({fn, user, owner});
    }
    ReleaseSRWLockExclusive(&g_tasksLock);

    if (refused && *refused) {
        LogF("%ls: AddTask refused: %s", Log_ModuleName(owner).c_str(), refused);
    }
    return refused ? DK2ML_ERROR : DK2ML_OK;
}

void Events_RunTasks()
{
    // take the whole queue: tasks queued meanwhile wait for the next frame
    std::vector<Task> due;
    AcquireSRWLockExclusive(&g_tasksLock);
    due.swap(g_tasks);
    ReleaseSRWLockExclusive(&g_tasksLock);

    for (const Task& t : due) {
        AcquireSRWLockShared(&g_tasksLock);
        bool faulted = IsFaulted(t.owner); // an earlier task may have switched it off
        ReleaseSRWLockShared(&g_tasksLock);
        if (!faulted && !CallTask(t)) {
            Contain(t.owner, "task (AddTask)");
        }
    }
}

void Events_Frame(void* gameClient, int64_t state)
{
    Events_RunTasks();

    if (state != g_lastState) {
        int64_t old = g_lastState;
        g_lastState = state;
        Events_Dispatch(DK2ML_EVENT_STATE_CHANGED, gameClient, old, state);
    }

    Events_Dispatch(DK2ML_EVENT_FRAME, gameClient, state, state);
}

std::vector<std::string> Events_TypesOf(HMODULE owner)
{
    std::vector<std::string> types;
    for (int type = DK2ML_EVENT_FRAME; type < kEventTypes; ++type) {
        for (const auto& s : g_subscribers[type]) {
            if (s.owner == owner) {
                types.push_back(TypeName(static_cast<DK2ML_EventType>(type)));
                break;
            }
        }
    }

    std::string gui;
    for (uint32_t id : Events_GuiEventIds()) {
        for (const auto& g : g_guiSubscribers) {
            if (g.id == id && g.s.owner == owner) {
                gui += (gui.empty() ? "" : ",") + std::to_string(id);
                break;
            }
        }
    }

    if (!gui.empty()) {
        types.push_back("GUI_EVENT " + gui);
    }
    return types;
}

void Events_LogSubscribers()
{
    for (int type = DK2ML_EVENT_FRAME; type < kEventTypes; ++type) {
        if (g_subscribers[type].empty()) {
            continue;
        }
        std::wstring names;
        for (const auto& s : g_subscribers[type]) {
            names += (names.empty() ? L"" : L", ") + Log_ModuleName(s.owner);
        }
        LogF("event %s: %ls", TypeName(static_cast<DK2ML_EventType>(type)), names.c_str());
    }

    for (uint32_t id : Events_GuiEventIds()) {
        std::wstring names;
        for (const auto& g : g_guiSubscribers) {
            if (g.id == id) {
                names += (names.empty() ? L"" : L", ") + Log_ModuleName(g.s.owner);
            }
        }
        LogF("GUI event %u: %ls", id, names.c_str());
    }
}
