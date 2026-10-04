// The game names the loader itself uses, resolved from the PDB the way plugins resolve theirs. Each group resolves on
// its own, so a missing name costs only that group.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gameui {

struct Functions {
    void* imguiRender; // ImGui::Render. Required by the menu; GameHooks.cpp hooks ev.imguiRender.
    void* (*FindChild)(void* item, const char* name);
    void (*ItemShow)(void* item);
    void (*ItemHide)(void* item);

    // the screen is real game GUI, merged from an XML document in memory while the GUI loads
    void* guiLoad; // GUIManager::Load. Required by the menu; GameHooks.cpp hooks ev.guiLoad.
    int (*MergeItemsFromXML)(void* manager, const void* document); // hooked by NativeModsButton.cpp
    void* (*XmlDocumentCtor)(void* document, bool processEntities, int whitespace);
    // keeps buffer; writes buffer[length]
    int (*XmlSetContentsAndLoadFromMem)(void* document, char* buffer, int length);
    void (*ActionExecute)(void* action); // GUI::sAction::Execute: re-parenting the main-menu button

    // widgets
    void (*CheckboxSetState)(void* checkbox, int state, bool runActions);
    float (*SliderGetValue)(const void* slider);
    void (*SliderSetValue)(void* slider, float value); // also sets the slider's tooltip to the value
    bool (*StaticTextChangeText)(void* text, const char* utf8); // text starting with '@' is a localization key

    // Required by the menu (event 219, see GuiKit.cpp). GuiKit.cpp registers through svc.RegisterConsumer.
    void (*EventRegisterConsumer)(void* eventSystem, void* consumer, unsigned eventId);
};

struct Offsets {
    int32_t guiRoot;          // GUIManager::m_pRoot
    int32_t itemHidden;       // GUI::Item::m_hidden
    int32_t itemParent;       // GUI::Item::m_parent
    int32_t buttonTexts;      // GUI::Button::m_pStaticText: StaticText*[3] (normal, hover, pushed; any may be null)

    // GUI::sAction
    int32_t actionOwner;
    int32_t actionType;
    int32_t actionTargetName;
    int32_t actionTarget;
    uint32_t actionSize;
    int64_t actionAddChild;   // GUI::eAction::ACTION_ADD_CHILD

    uint32_t xmlDocumentSize; // sizeof(tinyxml2::XMLDocument)
    int64_t eventGameLast;    // GUI::Events::eEventType::GUI_GAME_last (219)
    int32_t eventParamsInt;   // GUI::sEventParams::iParam1
};

struct Globals {
    void** guiManager;  // g_pGUIManager
    void** eventSystem; // g_eventSystemGUI. Required by the menu; GuiKit.cpp uses svc.eventSystem.
};

// The names behind the events (FRAME, GUI_LOADED, STATE_CHANGED, MAP_LOADED) and GetGameState. Resolved apart from
// the menu, so a missing name in one doesn't affect the other.
struct EventNames {
    void* imguiRender; // ImGui::Render: every frame, front-end included, except in random-map generation (state 6)
    void* guiLoad; // GUIManager::Load
    void** gameClient; // g_pGameClient
    int32_t clientState; // GameClient::m_state (eCGameState)
    int32_t clientCamera; // GameClient::m_camera: the view camera, a Camera inside the GameClient
    void* cameraSetDefaults; // Camera::SetDefaults: the game resets the view camera with it on every map load

    // which events have what they need
    bool frame;
    bool guiLoaded;
    bool state;
    bool mapLoaded;
};

// The GUI kit (DK2ML_API::Gui*, GuiKit.cpp): resolved on its own, so a missing name costs only the kit.
struct KitNames {
    void** guiManager; // g_pGUIManager
    int32_t guiRoot; // GUIManager::m_pRoot
    void* (*FindChild)(void* item, const char* name);
    void (*ItemShow)(void* item);
    void (*ItemHide)(void* item);
    void (*ActionExecute)(void* action);
    bool (*ChangeText)(void* text, const char* utf8);
    void (*ExecuteOnEvent)(void* item, int itemEvent, uint64_t cursor); // protected; Vector2 by value fits in rdx
    const void* buttonVtable; // GUI::Button::`vftable'
    const void* staticTextVtable; // GUI::StaticText::`vftable'

    int32_t itemHidden;
    int32_t itemParent;
    int32_t itemChildren;
    int32_t itemName;
    int32_t hashedStringText;
    int32_t itemEvents;
    int32_t buttonTexts;

    // LinkedList<GUI::Item>: m_children is a head node, each child has a node
    int32_t linkHead;
    int32_t linkNext;
    int32_t linkOwner;

    uint32_t eventPropsSize; // GUI::Item::sItemEventProperties, one per eItemEventType in m_eventProperties
    // its List<GUI::sAction *>
    int32_t eventPropsActions;
    int32_t listData;
    int32_t listCount;

    // GUI::sAction
    uint32_t actionSize;
    int32_t actionOwner;
    int32_t actionType;
    int32_t actionTargetName;
    int32_t actionTarget;
    int32_t actionParams;
    int32_t actionCallback;
    int32_t actionEventParams;
    int32_t eventParamsCursor; // GUI::sEventParams::cursor (Vector2)

    // eAction / eItemEventType values
    int64_t addChild;
    int64_t setOrigin;
    int64_t callback;
    int64_t eventClick;
    int64_t eventCount;

    bool ok;
};

// The loader's other game services: input capture, GUI events, WINDOW_RESIZED, the game version.
struct ServiceNames {
    void* isAnyMenuOpened; // GameGUI::IsAnyMenuOpened: hooked for CaptureGameInput
    void** gameGui; // g_pGameGUI
    void** eventSystem; // g_eventSystemGUI
    void (*RegisterConsumer)(void* eventSystem, void* consumer, unsigned eventId);
    int64_t guiEventCount; // GUI::Events::eEventType::NUM_VALUES
    void* onWindowResized; // GameRenderer::OnWindowResized
};

// The game's list of active mods. Mods::SetModAsActive edits it on every Mods-menu click, and the game then reloads
// its data and GUI in-process. Plugins_SyncEnabled compares it with the loaded mods on each GUI load.
struct ModListNames {
    void* instance; // g_modsInstance (a Mods)
    int32_t activeMods; // Mods::m_activeMods: List<StaticString<512> >, the mod folders as options.xml lists them
    int32_t listData; // List::m_list: the entries
    int32_t listCount; // List::m_elements: how many (m_size is the capacity)
    int32_t entryText; // StaticString<512>::m_szString
    uint32_t entrySize; // sizeof(StaticString<512>): the text, then its hash
    bool ok;
};

extern Functions fn;
extern Offsets off;
extern Globals globals;
extern EventNames ev;
extern KitNames kit;
extern ServiceNames svc;
extern ModListNames modList;

// Resolves modList. Logs what's missing; without it the loader re-reads options.xml instead.
bool ResolveModList();
// The game's current active mod folders, as it stores them (mixed slashes). False if modList isn't resolved or the list
// couldn't be read.
bool ActiveModPaths(std::vector<std::string>* out);

// Resolve kit / svc. Log what's missing; the service lacking a name is unavailable, nothing else.
bool ResolveKit();
void ResolveServices();
// The game's version, read from the code of its save functions (Roster::Save, ...). The game has no variable for it;
// they pass it to SetAttribute("gameVersion", 112) as an immediate. 0 if not found.
uint32_t FindGameVersion();

// Resolves fn, off and globals, the names the Native mods button and screen need (needs Symbols_Init). Logs what's
// missing and returns false; the loader then runs without its screen, and plugins are unaffected.
bool Resolve();

// Resolves ev. Logs what's missing; the events that lack a name are never sent.
void ResolveEvents();

template <typename T> T& Field(void* base, int32_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

} // namespace gameui
