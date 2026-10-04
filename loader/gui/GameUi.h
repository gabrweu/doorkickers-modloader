// Game names the loader uses, from the PDB. Groups resolve separately: a missing name costs only its group.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gameui {

struct Functions {
    void* imguiRender; // only gates the menu (hooked via ev.imguiRender)
    void* (*FindChild)(void* item, const char* name);
    void (*ItemShow)(void* item);
    void (*ItemHide)(void* item);

    // the screen: XML merged from memory during GUI load
    void* guiLoad; // only gates the menu (hooked via ev.guiLoad)
    int (*MergeItemsFromXML)(void* manager, const void* document); // hooked by NativeModsButton.cpp
    void* (*XmlDocumentCtor)(void* document, bool processEntities, int whitespace);
    // keeps buffer; writes buffer[length]
    int (*XmlSetContentsAndLoadFromMem)(void* document, char* buffer, int length);
    void (*ActionExecute)(void* action); // re-parents the main-menu button

    // widgets
    void (*CheckboxSetState)(void* checkbox, int state, bool runActions);
    float (*SliderGetValue)(const void* slider);
    void (*SliderSetValue)(void* slider, float value); // also sets the tooltip to the value
    bool (*StaticTextChangeText)(void* text, const char* utf8); // leading '@': localization key

    // only gates the menu (event 219); GuiKit.cpp uses svc.RegisterConsumer
    void (*EventRegisterConsumer)(void* eventSystem, void* consumer, unsigned eventId);
};

struct Offsets {
    int32_t guiRoot;
    int32_t itemHidden;
    int32_t itemParent;
    int32_t buttonTexts;      // StaticText*[3]: normal, hover, pushed; may be null

    // GUI::sAction
    int32_t actionOwner;
    int32_t actionType;
    int32_t actionTargetName;
    int32_t actionTarget;
    uint32_t actionSize;
    int64_t actionAddChild;

    uint32_t xmlDocumentSize;
    int64_t eventGameLast;    // GUI_GAME_last (219)
    int32_t eventParamsInt;   // sEventParams::iParam1
};

struct Globals {
    void** guiManager;
    void** eventSystem; // only gates the menu (GuiKit.cpp uses svc.eventSystem)
};

// The names behind FRAME, GUI_LOADED, STATE_CHANGED, MAP_LOADED and GetGameState; resolved apart from the menu.
struct EventNames {
    void* imguiRender; // every frame, front-end included, except random-map generation (state 6)
    void* guiLoad;
    void** gameClient;
    int32_t clientState; // eCGameState
    int32_t clientCamera; // the view camera, inside GameClient
    void* cameraSetDefaults; // resets the view camera on every map load

    // which events have their names
    bool frame;
    bool guiLoaded;
    bool state;
    bool mapLoaded;
};

// The GUI kit (DK2ML_API::Gui*, GuiKit.cpp).
struct KitNames {
    void** guiManager;
    int32_t guiRoot;
    void* (*FindChild)(void* item, const char* name);
    void (*ItemShow)(void* item);
    void (*ItemHide)(void* item);
    void (*ActionExecute)(void* action);
    bool (*ChangeText)(void* text, const char* utf8);
    void (*ExecuteOnEvent)(void* item, int itemEvent, uint64_t cursor); // protected; Vector2 by value fits in rdx
    const void* buttonVtable;
    const void* staticTextVtable;

    int32_t itemHidden;
    int32_t itemParent;
    int32_t itemChildren;
    int32_t itemName;
    int32_t hashedStringText;
    int32_t itemEvents;
    int32_t buttonTexts;

    // LinkedList<GUI::Item>: m_children is a head node; each child has a node
    int32_t linkHead;
    int32_t linkNext;
    int32_t linkOwner;

    uint32_t eventPropsSize; // one per eItemEventType in m_eventProperties
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
    int32_t eventParamsCursor; // Vector2

    // eAction / eItemEventType values
    int64_t addChild;
    int64_t setOrigin;
    int64_t callback;
    int64_t eventClick;
    int64_t eventCount;

    bool ok;
};

// Input capture, GUI events, WINDOW_RESIZED, the game version.
struct ServiceNames {
    void* isAnyMenuOpened; // hooked for CaptureGameInput
    void** gameGui;
    void** eventSystem;
    void (*RegisterConsumer)(void* eventSystem, void* consumer, unsigned eventId);
    int64_t guiEventCount; // eEventType::NUM_VALUES
    void* onWindowResized;
};

// The game's active mod list, edited on every Mods-menu click (Mods::SetModAsActive). See Plugins_SyncEnabled.
struct ModListNames {
    void* instance; // g_modsInstance
    int32_t activeMods; // List<StaticString<512> >: folders as options.xml lists them
    int32_t listData;
    int32_t listCount; // m_elements (m_size is the capacity)
    int32_t entryText;
    uint32_t entrySize; // the text, then its hash
    bool ok;
};

extern Functions fn;
extern Offsets off;
extern Globals globals;
extern EventNames ev;
extern KitNames kit;
extern ServiceNames svc;
extern ModListNames modList;

// Without it the loader re-reads options.xml.
bool ResolveModList();
// Active mod folders as the game stores them (mixed slashes). False if unresolved or unreadable.
bool ActiveModPaths(std::vector<std::string>* out);

// A missing name disables only its service.
bool ResolveKit();
void ResolveServices();
// From the save functions' code: no game variable holds it. 0 if not found.
uint32_t FindGameVersion();

// The Native mods button and screen (needs Symbols_Init). False: no screen; plugins unaffected.
bool Resolve();

// Events lacking a name are never sent.
void ResolveEvents();

template <typename T> T& Field(void* base, int32_t offset)
{
    return *reinterpret_cast<T*>(static_cast<char*>(base) + offset);
}

} // namespace gameui
