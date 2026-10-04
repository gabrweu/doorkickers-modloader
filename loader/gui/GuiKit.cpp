// The GUI kit (DK2ML_API::Gui*): lookups from the GUI root, show/hide, text, the AddChild/SetOrigin actions, the child
// list walk and Callback actions, all through the game's own functions and actions. The layouts and names live here,
// so a game update is fixed in one place.
// Also the game window, the game version, and the loader's one consumer of the game's GUI events. It takes plugins'
// SubscribeGuiEvent ids and event 219 (GUI_GAME_last). The game has no consumer for 219, so the Native mods screen's
// widgets send <Action type="TriggerEvent" target="GUI_GAME_last" iParam="N"/> and OnGameEvent passes it to the screen.
#include "Loader.h"

#include <algorithm>
#include <cstdio>

#include "GameUi.h"
#include "nativemods/NativeModsScreen.h"

using namespace gameui;

namespace {

bool g_initDone = false; // GuiKit_Init ran (after plugin init, before any GUI exists)
bool g_ready = false;
bool g_toldUnavailable = false;

bool Ready()
{
    if (!g_ready && g_initDone && !g_toldUnavailable) {
        g_toldUnavailable = true;
        LogF("GUI kit: unavailable in this game build (missing names above): the Gui functions do nothing");
    }
    return g_ready;
}

const void* VtableOf(const void* item)
{
    return *static_cast<const void* const*>(item);
}

// Runs a hand-built GUI::sAction, the same as an XML <Action> owned by `owner`. params must outlive the call.
void RunAction(int64_t type, void* owner, void* target, const char* params)
{
    std::vector<char> action(kit.actionSize, 0);
    Field<void*>(action.data(), kit.actionOwner) = owner;
    Field<int>(action.data(), kit.actionType) = static_cast<int>(type);
    Field<const char*>(action.data(), kit.actionTargetName) = "";
    Field<void*>(action.data(), kit.actionTarget) = target;
    Field<const char*>(action.data(), kit.actionParams) = params;
    kit.ActionExecute(action.data());
}

// --- the game's GUI events: one consumer for every id someone wants ---

// The game calls consumer->vtable[0](consumer, eventId, params). IEventConsumer::OnEvent returns void, and
// EventSystem::TriggerEvent calls every consumer of the id (newest first), so no consumer can stop an event.
void OnGameEvent(void*, unsigned id, void* params)
{
    if (id == static_cast<unsigned>(off.eventGameLast) && off.eventGameLast > 0) {
        Screen_OnGameEvent(params);
    }

    void* client = nullptr;
    int64_t state = GameHooks_GameState(&client);
    Events_DispatchGui(id, params, client, state);
}

struct Consumer {
    void* const* vtable;
};

void* const g_consumerVtable[] = {reinterpret_cast<void*>(&OnGameEvent)};
Consumer g_consumer = {g_consumerVtable};

// the game's main window: this process's largest visible top-level window without an owner
BOOL CALLBACK FindLargest(HWND hwnd, LPARAM param)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER)) {
        return TRUE;
    }

    auto* best = reinterpret_cast<std::pair<HWND, LONG>*>(param);
    RECT r;
    if (!GetClientRect(hwnd, &r)) {
        return TRUE;
    }
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > best->second) {
        *best = {hwnd, area};
    }
    return TRUE;
}

} // namespace

void GuiKit_Init()
{
    g_ready = ResolveKit();
    g_initDone = true;

    GuiCallbackLayout layout = {-1, -1};
    if (g_ready) {
        layout.actionOwner = kit.actionOwner;
        layout.actionCursor = kit.actionEventParams + kit.eventParamsCursor;
    }
    Gui_SetCallbackLayout(layout);

    ResolveServices();
}

void GuiKit_OnGuiLoaded(bool screen)
{
    // GUIManager::Load emptied the game's consumer table (EventSystem::Destroy + Init): register again every load.
    // GameGUI registers for its events after Load returns, so its handlers run before this consumer.
    if (!svc.eventSystem || !*svc.eventSystem || !svc.RegisterConsumer) {
        return;
    }

    std::vector<uint32_t> ids = Events_GuiEventIds();
    if (screen && off.eventGameLast > 0) {
        ids.push_back(static_cast<uint32_t>(off.eventGameLast));
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());

    static bool toldRange = false;
    for (uint32_t id : ids) {
        bool pastLast = svc.guiEventCount > 0 && id >= static_cast<uint64_t>(svc.guiEventCount);
        if (pastLast) {
            if (!toldRange) {
                LogF("GUI events: id %u is past this game build's last (%lld): its subscribers won't get it", id,
                     static_cast<long long>(svc.guiEventCount) - 1);
            }
            continue;
        }
        svc.RegisterConsumer(*svc.eventSystem, &g_consumer, id);
    }
    toldRange = true;
}

bool GuiKit_GuiEventsAvailable()
{
    return svc.eventSystem && svc.RegisterConsumer;
}

uint32_t GuiKit_GameVersion()
{
    static const uint32_t version = FindGameVersion(); // the code doesn't change, so look once (any thread)
    return version;
}

void* GuiKit_GameWindow()
{
    static HWND cached = nullptr;
    HWND w = cached;
    if (w && IsWindow(w) && IsWindowVisible(w)) {
        return w;
    }

    std::pair<HWND, LONG> best = {nullptr, 0};
    EnumWindows(FindLargest, reinterpret_cast<LPARAM>(&best));
    cached = best.first;
    return best.first;
}

// --- DK2ML_API::Gui* ---

void* Gui_Find(void* under, const char* name)
{
    if (!name || !Ready()) {
        return nullptr;
    }
    if (!under) {
        void* manager = *kit.guiManager;
        under = manager ? Field<void*>(manager, kit.guiRoot) : nullptr;
        if (!under) {
            return nullptr;
        }
    }
    return kit.FindChild(under, name);
}

void* Gui_Parent(void* item)
{
    return item && Ready() ? Field<void*>(item, kit.itemParent) : nullptr;
}

const char* Gui_Name(void* item)
{
    if (!item || !Ready()) {
        return "";
    }
    const char* name = Field<const char*>(item, kit.itemName + kit.hashedStringText);
    return name ? name : "";
}

int Gui_Children(void* item, void** out, int max)
{
    if (!item || !Ready()) {
        return 0;
    }

    // m_children is the head node of an intrusive circular list; each child's own node (a LinkedList<GUI::Item> base)
    // points back to it through `owner`. Walked the way GUI::ItemList::Init walks it.
    char* list = &Field<char>(item, kit.itemChildren);
    void* head = Field<void*>(list, kit.linkHead);
    void* node = Field<void*>(list, kit.linkNext);
    int n = 0;
    for (int guard = 0; node && node != head && guard < 100000; ++guard) {
        if (void* child = Field<void*>(node, kit.linkOwner)) {
            if (out && n < max) {
                out[n] = child;
            }
            ++n;
        }
        node = Field<void*>(node, kit.linkNext);
    }
    return n;
}

int Gui_IsShown(void* item)
{
    return item && Ready() && !Field<bool>(item, kit.itemHidden);
}

void Gui_Show(void* item, int show)
{
    if (!item || !Ready()) {
        return;
    }
    if (show) {
        kit.ItemShow(item);
    } else {
        kit.ItemHide(item);
    }
}

DK2ML_Status Gui_SetText(void* item, const char* utf8)
{
    if (!item || !utf8 || !Ready()) {
        return DK2ML_ERROR;
    }
    while (*utf8 == '@') { // '@' starts a game text key; mod text is shown as is
        ++utf8;
    }

    const void* vt = VtableOf(item);
    if (vt == kit.staticTextVtable) {
        kit.ChangeText(item, utf8);
        return DK2ML_OK;
    }
    if (vt == kit.buttonVtable) {
        void** texts = &Field<void*>(item, kit.buttonTexts); // normal, hover, pushed; any may be null
        for (int i = 0; i < 3; ++i) {
            if (texts[i]) {
                kit.ChangeText(texts[i], utf8);
            }
        }
        return DK2ML_OK;
    }
    return DK2ML_ERROR;
}

DK2ML_Status Gui_AddChild(void* parent, void* item)
{
    if (!parent || !item || !Ready()) {
        return DK2ML_ERROR;
    }
    RunAction(kit.addChild, parent, item, nullptr);
    return Field<void*>(item, kit.itemParent) == parent ? DK2ML_OK : DK2ML_ERROR;
}

DK2ML_Status Gui_SetOrigin(void* item, float x, float y)
{
    if (!item || !Ready()) {
        return DK2ML_ERROR;
    }
    char params[64]; // the action parses it with "%f %f" during the call
    snprintf(params, sizeof(params), "%.2f %.2f", x, y);
    RunAction(kit.setOrigin, item, item, params);
    return DK2ML_OK;
}

DK2ML_Status Gui_Click(void* item)
{
    if (!item || !Ready()) {
        return DK2ML_ERROR;
    }
    kit.ExecuteOnEvent(item, static_cast<int>(kit.eventClick), 0);
    return DK2ML_OK;
}

DK2ML_Status Gui_SetCallback(HMODULE owner, void* item, int itemEvent, DK2ML_GuiCallbackFn fn, void* user)
{
    if (!item || !fn || !Ready() || itemEvent < 0 || itemEvent >= kit.eventCount) {
        return DK2ML_ERROR;
    }

    char* props = &Field<char>(item, kit.itemEvents + static_cast<int32_t>(itemEvent * kit.eventPropsSize));
    char* list = props + kit.eventPropsActions;
    void** actions = Field<void**>(list, kit.listData);
    int count = Field<int>(list, kit.listCount);

    void* thunk = nullptr;
    int set = 0;
    for (int i = 0; actions && i < count; ++i) {
        void* a = actions[i];
        if (!a || Field<int>(a, kit.actionType) != static_cast<int>(kit.callback)) {
            continue;
        }
        if (!thunk) {
            thunk = Gui_CallbackThunk(owner, fn, user);
            if (!thunk) {
                return DK2ML_ERROR;
            }
        }
        Field<void*>(a, kit.actionCallback) = thunk;
        ++set;
    }
    return set ? DK2ML_OK : DK2ML_ERROR;
}
