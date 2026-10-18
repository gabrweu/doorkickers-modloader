// AddMenuButton: plugins' buttons in the Esc menus; called from Plugins.cpp and NativeModsButton.cpp.
//
// Before AddMenuButton, each mod moved its own items into the menus and picked its own spot, and they overlapped. Now
// the loader makes the items (MenuButtonsXml.cpp) and merges them with the Modloader button's document, so their
// Callback actions pass the load's target check. Each frame, every button is moved into each Esc menu's "Extra
// Buttons" row, bound to the plugin's onClick (a GuiThunks.cpp callback: crash-contained) and given the next free slot.
// A switched-off plugin's buttons are hidden and the rest close up.
#include "Loader.h"

#include <cstring>

#include "gui/GameUi.h"
#include "MenuButtonsXml.h"

using namespace gameui;

namespace {

namespace mx = menuxml;

struct Placed { // one button in one menu, for the current GUI load
    void* container = nullptr; // found once merged
    int slot = -1; // -1 until placed
    bool failed = false; // couldn't attach: left hidden
};

struct Entry {
    HMODULE owner;
    mx::Button xml;
    DK2ML_GuiCallbackFn fn;
    void* user;
    bool faulted; // owner switched off: hidden from then on
    Placed placed[mx::kMenuCount];
};

struct Menu { // per Esc menu, for the current GUI load
    void* row = nullptr;
    bool gaveUp = false; // no row: a UI mod changed the menu
};

// Added only during init; the main thread reads them lock-free after. The lock is for MenuButtons_FaultOwner, which
// runs on the crashing plugin's thread.
std::vector<Entry> g_entries;
SRWLOCK g_lock = SRWLOCK_INIT;

Menu g_menus[mx::kMenuCount];
bool g_logged[mx::kMenuCount] = {}; // once per session
bool g_loggedFull[mx::kMenuCount] = {};

std::string Copy(const char* s)
{
    return s ? std::string(s, strnlen(s, mx::kMaxText)) : std::string();
}

// Returns whether the container is in the row (else it stays hidden at the top level).
bool Attach(Entry& e, int id, int m, void* row)
{
    Placed& p = e.placed[m];
    if (Gui_Parent(p.container) == row) {
        return true;
    }

    void* button = Gui_Find(p.container, mx::ButtonName(id, m).c_str());
    bool attached = button && Gui_AddChild(row, p.container) == DK2ML_OK &&
                    Gui_SetCallback(e.owner, button, static_cast<int>(kit.eventClick), e.fn, e.user) == DK2ML_OK;
    if (!attached) {
        LogF("%ls: couldn't attach its Esc-menu button to %s", Log_ModuleName(e.owner).c_str(), mx::kMenus[m]);
        Gui_Show(p.container, 0); // the row may have it now, unbound
        p.failed = true;
        return false;
    }

    p.slot = -1;
    Gui_Show(p.container, 1);
    return true;
}

void AttachAll(int m)
{
    Menu& menu = g_menus[m];
    if (menu.gaveUp) {
        return;
    }
    if (!menu.row) {
        void* item = Gui_Find(nullptr, mx::kMenus[m]);
        if (!item) {
            return; // not loaded (yet)
        }
        menu.row = Gui_Find(item, mx::kRow);
        if (!menu.row) {
            LogF("menu buttons: %s has no %s row (a UI mod?); plugins' buttons aren't shown there", mx::kMenus[m],
                 mx::kRow);
            menu.gaveUp = true;
            return;
        }
    }

    int slot = 0;
    int shown = 0;
    for (size_t i = 0; i < g_entries.size(); ++i) {
        Entry& e = g_entries[i];
        Placed& p = e.placed[m];
        int id = static_cast<int>(i);
        if (!p.container) {
            p.container = Gui_Find(nullptr, mx::ContainerName(id, m).c_str());
            if (!p.container) {
                continue; // not in this load's document (switched off before it), or the merge failed
            }
        }
        if (e.faulted) {
            if (Gui_IsShown(p.container)) {
                Gui_Show(p.container, 0);
            }
            continue;
        }
        if (p.failed || !Attach(e, id, m, menu.row)) {
            continue;
        }

        if (slot >= mx::kSlots) {
            if (Gui_IsShown(p.container)) {
                Gui_Show(p.container, 0);
            }
            p.slot = -1;
            if (!g_loggedFull[m]) {
                g_loggedFull[m] = true;
                LogF("menu buttons: %s is full (%d); %ls's and later buttons aren't shown", mx::kMenus[m], mx::kSlots,
                     Log_ModuleName(e.owner).c_str());
            }
            continue;
        }

        if (p.slot != slot) {
            float x, y;
            mx::SlotOrigin(slot, &x, &y);
            Gui_SetOrigin(p.container, x, y);
            p.slot = slot;
            if (!Gui_IsShown(p.container)) {
                Gui_Show(p.container, 1); // hidden while the menu was full
            }
        }
        ++slot;
        ++shown;
    }

    if (shown && !g_logged[m]) {
        g_logged[m] = true;
        LogF("menu buttons: %d attached to %s > %s", shown, mx::kMenus[m], mx::kRow);
    }
}

} // namespace

DK2ML_Status MenuButtons_Add(HMODULE owner, const DK2ML_MenuButton* button)
{
    DK2ML_MenuButton copy;
    if (const char* error = mx::Check(button, &copy)) {
        LogF("%ls: AddMenuButton \"%s\" ignored: %s", Log_ModuleName(owner).c_str(), Copy(copy.tooltip).c_str(), error);
        return DK2ML_ERROR;
    }

    // copies: the plugin's strings needn't outlive the call
    Entry e = {};
    e.owner = owner;
    e.xml = {0, Copy(copy.tooltip), Copy(copy.icon), Copy(copy.iconHover), Copy(copy.label)};
    e.fn = copy.onClick;
    e.user = copy.user;

    AcquireSRWLockExclusive(&g_lock);
    e.xml.id = static_cast<int>(g_entries.size());
    g_entries.push_back(e);
    ReleaseSRWLockExclusive(&g_lock);

    LogF("%ls: Esc-menu button %d added (%s)", Log_ModuleName(owner).c_str(), e.xml.id,
         e.xml.icon.empty() ? e.xml.label.c_str() : e.xml.icon.c_str());
    return DK2ML_OK;
}

void MenuButtons_FaultOwner(HMODULE owner)
{
    AcquireSRWLockShared(&g_lock);
    for (Entry& e : g_entries) {
        if (e.owner == owner) {
            e.faulted = true;
        }
    }
    ReleaseSRWLockShared(&g_lock);
}

std::string MenuButtons_BuildItems()
{
    std::vector<mx::Button> buttons;
    for (const Entry& e : g_entries) {
        if (!e.faulted) {
            buttons.push_back(e.xml);
        }
    }
    return mx::Items(buttons);
}

void MenuButtons_OnFrame()
{
    if (g_entries.empty()) {
        return;
    }
    for (int m = 0; m < mx::kMenuCount; ++m) {
        AttachAll(m);
    }
}

void MenuButtons_OnGuiLoadBegin()
{
    // a GUI reload makes new items, and a UI mod turned off may give a menu its row back
    for (Menu& menu : g_menus) {
        menu = {};
    }
    for (Entry& e : g_entries) {
        for (Placed& p : e.placed) {
            p = {};
        }
    }
}
