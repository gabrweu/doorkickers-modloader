// The loader's own game hooks, each hooked once, feeding Events.cpp and the Modloader button and screen.
// ImGui::Render and GUIManager::Load are hooked even without the menu (they drive events); Camera::SetDefaults only
// if MAP_LOADED is subscribed, because the renderer calls it many times per frame.
#include "Loader.h"

#include "gui/GameUi.h"

using namespace gameui;

namespace {

HMODULE Self()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Self), &self);
    return self;
}

// The game's active list is already current: SetModAsActive runs per click.
void SyncEnabledMods()
{
    if (!modList.ok) {
        Plugins_SyncEnabled(nullptr); // options.xml, as the game last saved it
        return;
    }

    std::vector<std::string> active;
    if (ActiveModPaths(&active)) {
        Plugins_SyncEnabled(&active);
    } else {
        LogF("mod list: couldn't read the game's list of active mods this time");
    }
}

int GuiLoadPre(DK2ML_Regs*, void*)
{
    SyncEnabledMods();
    Menu_OnGuiLoadBegin();
    return DK2ML_CALL_ORIGINAL;
}

void GuiLoadPost(DK2ML_Regs*, void*)
{
    Menu_OnGuiLoadEnd();
    GuiKit_OnGuiLoaded(Menu_Ready()); // Load wiped the event consumers: register again
    Menu_OnGuiLoaded();

    void* client = nullptr;
    int64_t state = GameHooks_GameState(&client);
    Events_Dispatch(DK2ML_EVENT_GUI_LOADED, client, state, state);
}

int FrameTickPre(DK2ML_Regs*, void*)
{
    Menu_OnFrame();

    void* client = nullptr;
    int64_t state = GameHooks_GameState(&client);
    Events_Frame(client, state);
    return DK2ML_CALL_ORIGINAL;
}

// MAP_LOADED: SetDefaults on GameClient's camera (every map load); the renderer's temporary cameras don't count.
int SetDefaultsPre(DK2ML_Regs* r, void*)
{
    r->scratch[0] = r->rcx; // the Camera
    return DK2ML_CALL_ORIGINAL;
}

void SetDefaultsPost(DK2ML_Regs* r, void*)
{
    void* client = nullptr;
    int64_t state = GameHooks_GameState(&client);
    if (client && r->scratch[0] == reinterpret_cast<uint64_t>(client) + ev.clientCamera) {
        Events_Dispatch(DK2ML_EVENT_MAP_LOADED, client, state, state);
    }
}

// CaptureGameInput: IsAnyMenuOpened returns true while anyone captures; IsGameMenuOpen asks without them.
thread_local bool t_askingGameOnly = false;

int IsAnyMenuOpenedPre(DK2ML_Regs* r, void*)
{
    if (t_askingGameOnly || !Gui_AnyCapture()) {
        return DK2ML_CALL_ORIGINAL;
    }
    r->rax = (r->rax & ~0xFFull) | 1; // bool true
    return DK2ML_SKIP_ORIGINAL;
}

// WINDOW_RESIZED: the window's client size (GetClientRect)
void OnWindowResizedPost(DK2ML_Regs*, void*)
{
    RECT rect = {};
    HWND window = static_cast<HWND>(GuiKit_GameWindow());
    if (!window || !GetClientRect(window, &rect)) {
        return;
    }

    void* client = nullptr;
    int64_t state = GameHooks_GameState(&client);
    Events_DispatchResize(client, state, rect.right - rect.left, rect.bottom - rect.top);
}

bool g_captureHooked = false;

bool AnyPluginRunning()
{
    bool anyRunning = false;
    for (const ModEntry& e : Plugins_Mods()) {
        anyRunning |= e.status == ModStatus::Loaded;
    }
    return anyRunning;
}

// plugins made for another game version: shown, not enforced
void LogGameVersion()
{
    uint32_t version = GuiKit_GameVersion();
    LogF("game version: %u%s", version, version ? "" : " (unknown)");

    for (const ModEntry& e : Plugins_Mods()) {
        for (size_t k = 0; k < e.manifests.size() && k < e.dllNames.size(); ++k) {
            uint32_t madeFor = e.manifests[k].gameVersion;
            bool otherVersion = version && madeFor && madeFor != version;
            if (otherVersion) {
                LogF("%ls says it was made for game version %u (this is %u): if it misbehaves, look for an update",
                     e.dllNames[k].c_str(), madeFor, version);
            }
        }
    }
}

} // namespace

bool GameHooks_Install(void* target, DK2ML_PreFn pre, DK2ML_PostFn post, const char* what)
{
    bool created = SafeHook_Create(target, pre, post, nullptr, Self()) == DK2ML_OK;
    bool ok = created && SafeHook_SetEnabled(target, Self(), true) > 0;
    if (!ok) {
        LogF("cannot hook %s (%p)", what, target);
    }
    return ok;
}

int GameHooks_IsGameMenuOpen()
{
    bool haveGameGui = svc.isAnyMenuOpened && svc.gameGui && *svc.gameGui;
    if (!haveGameGui) {
        return 0;
    }

    t_askingGameOnly = true;
    bool open = reinterpret_cast<bool (*)(void*)>(svc.isAnyMenuOpened)(*svc.gameGui);
    t_askingGameOnly = false;
    return open ? 1 : 0;
}

int64_t GameHooks_GameState(void** gameClient)
{
    void* client = ev.gameClient ? *ev.gameClient : nullptr;
    if (gameClient) {
        *gameClient = client;
    }

    if (!client || ev.clientState < 0) {
        return -1;
    }
    return Field<int>(client, ev.clientState);
}

void GameHooks_Init()
{
    gameui::ResolveEvents();
    if (!gameui::ResolveModList()) {
        LogF("mod list: the game's list of active mods isn't available in this build: Mods menu changes are read from "
             "options.xml instead");
    }
    GuiKit_Init();

    bool frame = ev.frame && GameHooks_Install(ev.imguiRender, FrameTickPre, nullptr, "the frame tick, ImGui::Render");
    if (!frame) {
        Events_SetTasksAvailable(false); // AddTask's tasks run from the frame tick
    }
    bool guiLoad = ev.guiLoaded && GameHooks_Install(ev.guiLoad, GuiLoadPre, GuiLoadPost, "GUIManager::Load");
    Menu_Init(frame && guiLoad);

    bool mapLoadedWanted = ev.mapLoaded && Events_Wanted(DK2ML_EVENT_MAP_LOADED);
    bool mapLoaded = mapLoadedWanted &&
                     GameHooks_Install(ev.cameraSetDefaults, SetDefaultsPre, SetDefaultsPost, "Camera::SetDefaults");
    bool resizedWanted = svc.onWindowResized && Events_Wanted(DK2ML_EVENT_WINDOW_RESIZED);
    bool resized = resizedWanted && GameHooks_Install(svc.onWindowResized, nullptr, OnWindowResizedPost,
                                                      "GameRenderer::OnWindowResized");

    // CaptureGameInput is a runtime call, so hook whenever plugins run
    bool canCapture = svc.isAnyMenuOpened && svc.gameGui;
    if (AnyPluginRunning() && canCapture) {
        g_captureHooked =
            GameHooks_Install(svc.isAnyMenuOpened, IsAnyMenuOpenedPre, nullptr, "GameGUI::IsAnyMenuOpened");
    }

    // a subscribed event this build can't provide never arrives: log it
    struct {
        DK2ML_EventType type;
        bool available;
        const char* name;
    } events[] = {{DK2ML_EVENT_FRAME, frame, "FRAME"},
                  {DK2ML_EVENT_GUI_LOADED, guiLoad, "GUI_LOADED"},
                  {DK2ML_EVENT_STATE_CHANGED, frame && ev.state, "STATE_CHANGED"},
                  {DK2ML_EVENT_MAP_LOADED, mapLoaded, "MAP_LOADED"},
                  {DK2ML_EVENT_GUI_EVENT, guiLoad && GuiKit_GuiEventsAvailable(), "GUI_EVENT"},
                  {DK2ML_EVENT_WINDOW_RESIZED, resized, "WINDOW_RESIZED"}};

    for (const auto& e : events) {
        if (Events_Wanted(e.type) && !e.available) {
            LogF("events: %s isn't available in this game build (missing names above): its subscribers won't get it",
                 e.name);
        }
    }
    Events_LogSubscribers();

    LogGameVersion();
}
