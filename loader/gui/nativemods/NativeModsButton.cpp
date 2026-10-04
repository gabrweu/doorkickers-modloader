// The "Native mods" button (main menu, above "Send Feedback") and screen. GameHooks.cpp calls in from its frame tick
// and GUI-load hooks.
//
// The loader has no mod folder for a GUI file. So while GUIManager::Load merges the GUI files, one more document is
// merged from memory through GUIManager::MergeItemsFromXML, the game's own path for data\gui and mods' gui\ files.
// Its items get the same parsing and action-target checks. It holds the button and the whole "Native mods" screen,
// generated from the loader's records (NativeModsScreen.cpp / NativeModsScreenXml.cpp).
// Merged items land at the top level. Each frame the button is moved into Menu_Main > "Extra Buttons" (the row with
// Options, Mods, Replays and Send Feedback) with the game's AddChild action, so it shows and hides with the menu.
#include "Loader.h"

#include <cstring>

#include "gui/GameUi.h"
#include "NativeModsScreen.h"
#include "NativeModsScreenXml.h"

using namespace gameui;

namespace {

bool g_menuReady = false; // the menu's names resolved and its hooks are in

bool g_inGuiLoad = false;
bool g_mergedThisLoad = false;

bool g_attachGaveUp = false;
bool g_loggedAttach = false;

// A fresh document per GUI load, never freed. SetContentsAndLoadFromMem keeps the buffer and writes buffer[length], so
// it gets length + 1 bytes. A destructor would free the buffer with the game's heap. It costs a few KB per GUI load.
void* MakeDocument(const std::string& items)
{
    std::string text = "<GUIItems>" + screenxml::MainMenuButton(Plugins_RestartNeeded()) + items + "</GUIItems>";

    void* doc = VirtualAlloc(nullptr, off.xmlDocumentSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    char* buffer = static_cast<char*>(VirtualAlloc(nullptr, text.size() + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!doc || !buffer) {
        return nullptr;
    }

    memcpy(buffer, text.data(), text.size());
    fn.XmlDocumentCtor(doc, true, 0); // tinyxml2 defaults: process entities, preserve whitespace
    int error = fn.XmlSetContentsAndLoadFromMem(doc, buffer, static_cast<int>(text.size()));
    if (error != 0) {
        LogF("menu: the generated GUI XML doesn't parse (tinyxml2 error %d)", error);
        return nullptr;
    }
    return doc;
}

int MergeItemsPre(DK2ML_Regs* r, void*)
{
    r->scratch[0] = r->rcx; // the GUIManager
    return DK2ML_CALL_ORIGINAL;
}

// After the first GUI file of a load: merge ours too (before Load checks every action's target).
void MergeItemsPost(DK2ML_Regs* r, void*)
{
    if (!g_inGuiLoad || g_mergedThisLoad) {
        return;
    }
    g_mergedThisLoad = true; // also stops the merge below from re-entering here
    if (void* doc = MakeDocument(Screen_BuildItems())) {
        int result = fn.MergeItemsFromXML(reinterpret_cast<void*>(r->scratch[0]), doc);
        LogF("menu: Native mods button and screen %s", result == 0 ? "added" : "could not be added");
    }
}

// Keeps the button in Menu_Main > Extra Buttons (re-checked every frame, so a GUI reload just gets re-attached).
void AttachButton()
{
    void* manager = *globals.guiManager;
    void* root = manager ? Field<void*>(manager, off.guiRoot) : nullptr;
    if (!root || g_attachGaveUp) {
        return;
    }

    void* button = fn.FindChild(root, screenxml::kMainMenuButton);
    void* mainMenu = button ? fn.FindChild(root, "Menu_Main") : nullptr;
    if (!mainMenu) {
        return; // not merged (yet)
    }

    void* row = fn.FindChild(mainMenu, "Extra Buttons");
    if (!row) {
        LogF("menu: Menu_Main has no Extra Buttons row (a UI mod?); the button isn't shown");
        g_attachGaveUp = true;
        return;
    }

    if (Field<void*>(button, off.itemParent) != row) {
        // equivalent of <Action type="AddChild" target="#dk2ml_native_mods"/> owned by the row
        std::vector<char> action(off.actionSize, 0);
        Field<void*>(action.data(), off.actionOwner) = row;
        Field<int>(action.data(), off.actionType) = static_cast<int>(off.actionAddChild);
        Field<const char*>(action.data(), off.actionTargetName) = screenxml::kMainMenuButton;
        Field<void*>(action.data(), off.actionTarget) = button;
        fn.ActionExecute(action.data());
        if (Field<void*>(button, off.itemParent) != row) {
            LogF("menu: couldn't move the button into the main menu");
            g_attachGaveUp = true;
            return;
        }
    }

    if (Field<bool>(button, off.itemHidden)) {
        fn.ItemShow(button);
    }
    if (!g_loggedAttach) {
        g_loggedAttach = true;
        LogF("menu: button attached to Menu_Main > Extra Buttons");
    }
}

} // namespace

bool Menu_Init(bool hooksReady)
{
    g_menuReady = gameui::Resolve();
    if (!g_menuReady) {
        LogF("menu: not available for this game build (native mods still load)");
    }

    bool hooked = false;
    if (g_menuReady && hooksReady) {
        hooked = GameHooks_Install(reinterpret_cast<void*>(fn.MergeItemsFromXML), MergeItemsPre, MergeItemsPost,
                                   "GUIManager::MergeItemsFromXML");
    }
    if (hooked) {
        LogF("menu: ready (main menu > Native mods)");
    } else {
        g_menuReady = false;
    }
    return g_menuReady;
}

bool Menu_Ready()
{
    return g_menuReady;
}

void Menu_OnFrame()
{
    if (!g_menuReady) {
        return;
    }
    AttachButton();
    Screen_Tick();
}

void Menu_OnGuiLoadBegin()
{
    g_inGuiLoad = true;
    g_mergedThisLoad = false;
    g_attachGaveUp = false; // a GUI reload builds everything anew
}

void Menu_OnGuiLoadEnd()
{
    g_inGuiLoad = false;
}

void Menu_OnGuiLoaded()
{
    if (g_menuReady) {
        Screen_OnGuiLoaded();
    }
}
