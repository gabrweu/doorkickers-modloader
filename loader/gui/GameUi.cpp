#include "GameUi.h"

#include "Loader.h"

#include <algorithm>
#include <cstring>

namespace gameui {

Functions fn = {};
Offsets off = {};
Globals globals = {};
EventNames ev = {};
KitNames kit = {};
ServiceNames svc = {};
ModListNames modList = {};

namespace {

bool g_ok = true;

template <typename T> void Sym(const char* name, T* out)
{
    *out = reinterpret_cast<T>(Symbols_Resolve(name));
    if (!*out) {
        LogF("menu: missing symbol %s", name);
        g_ok = false;
    }
}

void Field(const char* type, const char* field, int32_t* out)
{
    *out = Symbols_FieldOffset(type, field);
    if (*out < 0) {
        LogF("menu: missing field %s::%s", type, field);
        g_ok = false;
    }
}

void Enum(const char* type, const char* name, int64_t* out)
{
    if (!Symbols_EnumValue(type, name, out)) {
        LogF("menu: missing enum %s::%s", type, name);
        g_ok = false;
    }
}

} // namespace

bool Resolve()
{
    g_ok = true;
    Sym("ImGui::Render", &fn.imguiRender);

    Sym("?FindChild@Item@GUI@@QEAAPEAV12@PEBD@Z", &fn.FindChild);
    Sym("?Show@Item@GUI@@UEAAXXZ", &fn.ItemShow);
    Sym("?Hide@Item@GUI@@UEAAXXZ", &fn.ItemHide);

    Sym("?Load@GUIManager@@QEAAHXZ", &fn.guiLoad);
    Sym("?MergeItemsFromXML@GUIManager@@IEAAHAEBVXMLDocument@tinyxml2@@@Z", &fn.MergeItemsFromXML);
    Sym("??0XMLDocument@tinyxml2@@QEAA@_NW4Whitespace@1@@Z", &fn.XmlDocumentCtor);
    Sym("?SetContentsAndLoadFromMem@XMLDocument@tinyxml2@@QEAA?AW4XMLError@2@PEADH@Z",
        &fn.XmlSetContentsAndLoadFromMem);
    Sym("GUI::sAction::Execute", &fn.ActionExecute);

    Sym("?SetState@Checkbox@GUI@@QEAAXW4eCheckboxState@12@_N@Z", &fn.CheckboxSetState);
    Sym("?GetValue@Slider@GUI@@QEBAMXZ", &fn.SliderGetValue);
    Sym("?SetValue@Slider@GUI@@QEAAXM@Z", &fn.SliderSetValue);
    Sym("?ChangeText@StaticText@GUI@@QEAA_NPEBD@Z", &fn.StaticTextChangeText);

    Sym("?RegisterConsumer@EventSystem@@QEAAXPEAVIEventConsumer@@I@Z", &fn.EventRegisterConsumer);

    Sym("g_pGUIManager", &globals.guiManager);
    Sym("?g_eventSystemGUI@@3PEAVEventSystem@@EA", &globals.eventSystem);

    Field("GUIManager", "m_pRoot", &off.guiRoot);
    Field("GUI::Item", "m_hidden", &off.itemHidden);
    Field("GUI::Item", "m_parent", &off.itemParent);
    Field("GUI::Button", "m_pStaticText", &off.buttonTexts);

    Field("GUI::sAction", "owner", &off.actionOwner);
    Field("GUI::sAction", "action", &off.actionType);
    Field("GUI::sAction", "targetName", &off.actionTargetName);
    Field("GUI::sAction", "target", &off.actionTarget);
    Field("GUI::sEventParams", "iParam1", &off.eventParamsInt);

    off.actionSize = Symbols_TypeSize("GUI::sAction");
    off.xmlDocumentSize = Symbols_TypeSize("tinyxml2::XMLDocument");
    if (!off.actionSize || !off.xmlDocumentSize) {
        LogF("menu: missing type GUI::sAction or tinyxml2::XMLDocument");
        g_ok = false;
    }

    Enum("GUI::eAction", "ACTION_ADD_CHILD", &off.actionAddChild);
    Enum("GUI::Events::eEventType", "GUI_GAME_last", &off.eventGameLast);
    return g_ok;
}

void ResolveEvents()
{
    auto sym = [](const char* name) {
        void* p = Symbols_Resolve(name);
        if (!p) {
            LogF("events: missing symbol %s", name);
        }
        return p;
    };

    auto field = [](const char* type, const char* name) {
        int32_t offset = Symbols_FieldOffset(type, name);
        if (offset < 0) {
            LogF("events: missing field %s::%s", type, name);
        }
        return offset;
    };

    ev.imguiRender = sym("ImGui::Render");
    ev.guiLoad = sym("?Load@GUIManager@@QEAAHXZ");
    ev.gameClient = static_cast<void**>(sym("g_pGameClient"));
    ev.clientState = field("GameClient", "m_state");
    ev.clientCamera = field("GameClient", "m_camera");
    ev.cameraSetDefaults = sym("Camera::SetDefaults");

    ev.frame = ev.imguiRender != nullptr;
    ev.guiLoaded = ev.guiLoad != nullptr;
    ev.state = ev.frame && ev.gameClient && ev.clientState >= 0;
    ev.mapLoaded = ev.gameClient && ev.clientCamera >= 0 && ev.cameraSetDefaults;
}

bool ResolveKit()
{
    bool ok = true;
    auto sym = [&](const char* name) {
        void* p = Symbols_Resolve(name);
        if (!p) {
            LogF("GUI kit: missing symbol %s", name);
            ok = false;
        }
        return p;
    };

    auto field = [&](const char* type, const char* name) {
        int32_t offset = Symbols_FieldOffset(type, name);
        if (offset < 0) {
            LogF("GUI kit: missing field %s::%s", type, name);
            ok = false;
        }
        return offset;
    };

    auto size = [&](const char* type) {
        uint32_t n = Symbols_TypeSize(type);
        if (!n) {
            LogF("GUI kit: missing type %s", type);
            ok = false;
        }
        return n;
    };

    auto value = [&](const char* type, const char* name) {
        int64_t v = -1;
        if (!Symbols_EnumValue(type, name, &v)) {
            LogF("GUI kit: missing enum %s::%s", type, name);
            ok = false;
        }
        return v;
    };

    // globals, functions and vtables
    kit.guiManager = static_cast<void**>(sym("g_pGUIManager"));
    kit.FindChild = reinterpret_cast<decltype(kit.FindChild)>(sym("?FindChild@Item@GUI@@QEAAPEAV12@PEBD@Z"));
    kit.ItemShow = reinterpret_cast<decltype(kit.ItemShow)>(sym("?Show@Item@GUI@@UEAAXXZ"));
    kit.ItemHide = reinterpret_cast<decltype(kit.ItemHide)>(sym("?Hide@Item@GUI@@UEAAXXZ"));
    kit.ActionExecute = reinterpret_cast<decltype(kit.ActionExecute)>(sym("GUI::sAction::Execute"));
    kit.ChangeText = reinterpret_cast<decltype(kit.ChangeText)>(sym("?ChangeText@StaticText@GUI@@QEAA_NPEBD@Z"));
    kit.ExecuteOnEvent = reinterpret_cast<decltype(kit.ExecuteOnEvent)>(
        sym("?ExecuteOnEvent@Item@GUI@@IEAAXW4eItemEventType@12@VVector2@@@Z"));
    kit.buttonVtable = sym("??_7Button@GUI@@6B@");
    kit.staticTextVtable = sym("??_7StaticText@GUI@@6B@");

    // items
    kit.guiRoot = field("GUIManager", "m_pRoot");
    kit.itemHidden = field("GUI::Item", "m_hidden");
    kit.itemParent = field("GUI::Item", "m_parent");
    kit.itemChildren = field("GUI::Item", "m_children");
    kit.itemName = field("GUI::Item", "m_hashedName");
    kit.hashedStringText = field("HashedString", "m_pString");
    kit.itemEvents = field("GUI::Item", "m_eventProperties");
    kit.buttonTexts = field("GUI::Button", "m_pStaticText");

    // the child list
    kit.linkHead = field("LinkedList<GUI::Item>", "head");
    kit.linkNext = field("LinkedList<GUI::Item>", "next");
    kit.linkOwner = field("LinkedList<GUI::Item>", "owner");

    // an item event's action list
    kit.eventPropsSize = size("GUI::Item::sItemEventProperties");
    kit.eventPropsActions = field("GUI::Item::sItemEventProperties", "actions");
    kit.listData = field("List<GUI::sAction *>", "m_list");
    kit.listCount = field("List<GUI::sAction *>", "m_elements");

    // actions
    kit.actionSize = size("GUI::sAction");
    kit.actionOwner = field("GUI::sAction", "owner");
    kit.actionType = field("GUI::sAction", "action");
    kit.actionTargetName = field("GUI::sAction", "targetName");
    kit.actionTarget = field("GUI::sAction", "target");
    kit.actionParams = field("GUI::sAction", "params");
    kit.actionCallback = field("GUI::sAction", "pCallback");
    kit.actionEventParams = field("GUI::sAction", "eventParams");
    kit.eventParamsCursor = field("GUI::sEventParams", "cursor");

    // enum values
    kit.addChild = value("GUI::eAction", "ACTION_ADD_CHILD");
    kit.setOrigin = value("GUI::eAction", "ACTION_SET_ORIGIN");
    kit.callback = value("GUI::eAction", "ACTION_CALLBACK");
    kit.eventClick = value("GUI::Item::eItemEventType", "EVENT_CLICK");
    kit.eventCount = value("GUI::Item::eItemEventType", "EVENT_NUM_VALUES");

    kit.ok = ok;
    return ok;
}

void ResolveServices()
{
    auto sym = [](const char* what, const char* name) {
        void* p = Symbols_Resolve(name);
        if (!p) {
            LogF("%s: missing symbol %s", what, name);
        }
        return p;
    };

    svc.isAnyMenuOpened = sym("input capture", "?IsAnyMenuOpened@GameGUI@@QEBA_NXZ");
    svc.gameGui = static_cast<void**>(sym("input capture", "?g_pGameGUI@@3PEAVGameGUI@@EA"));

    svc.eventSystem = static_cast<void**>(sym("GUI events", "?g_eventSystemGUI@@3PEAVEventSystem@@EA"));
    svc.RegisterConsumer = reinterpret_cast<decltype(svc.RegisterConsumer)>(
        sym("GUI events", "?RegisterConsumer@EventSystem@@QEAAXPEAVIEventConsumer@@I@Z"));
    if (!Symbols_EnumValue("GUI::Events::eEventType", "NUM_VALUES", &svc.guiEventCount)) {
        LogF("GUI events: missing enum GUI::Events::eEventType::NUM_VALUES");
        svc.guiEventCount = 0;
    }

    svc.onWindowResized = sym("WINDOW_RESIZED", "?OnWindowResized@GameRenderer@@QEAAXHHH@Z");
}

namespace {

// No destructors in here (__try).
bool ReadCode(const uint8_t* p, void* out, size_t n)
{
    __try {
        memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// `lea rdx, [rip+d]` (48 8D 15 d32) at p pointing at the string "gameVersion"
bool LeaGameVersion(const uint8_t* p)
{
    uint8_t b[7];
    if (!ReadCode(p, b, sizeof(b)) || b[0] != 0x48 || b[1] != 0x8D || b[2] != 0x15) {
        return false;
    }

    int32_t d;
    memcpy(&d, b + 3, 4);
    char text[12] = {};
    return ReadCode(p + 7 + d, text, sizeof(text)) && memcmp(text, "gameVersion", 12) == 0;
}

// The N of SetAttribute(element, "gameVersion", N) in a save function, or 0. N is a `mov r8d, imm32` (41 B8) next to
// the lea of the name (build 112: Roster::Save +0xC1).
uint32_t VersionIn(const char* function)
{
    auto* code = static_cast<const uint8_t*>(Symbols_Resolve(function));
    if (!code) {
        return 0;
    }

    constexpr int kSpan = 0x1000;
    constexpr int kNear = 24;
    for (int i = 0; i < kSpan; ++i) {
        if (!LeaGameVersion(code + i)) {
            continue;
        }
        for (int j = i - kNear; j < i + 7 + kNear; ++j) {
            uint8_t b[6];
            if (ReadCode(code + j, b, sizeof(b)) && b[0] == 0x41 && b[1] == 0xB8) {
                uint32_t v;
                memcpy(&v, b + 2, 4);
                if (v > 0 && v < 100000) {
                    return v;
                }
            }
        }
    }
    return 0;
}

} // namespace

uint32_t FindGameVersion()
{
    uint32_t found = 0;
    for (const char* f : {"Roster::Save", "Mods::sMod::SaveToFile", "MapSaves::Save", "UnlockedSaves::Save"}) {
        uint32_t v = VersionIn(f);
        if (!v) {
            continue;
        }
        if (found && v != found) {
            LogF("game version: %s says %u, another save function %u: not using either", f, v, found);
            return 0;
        }
        found = v;
    }

    if (!found) {
        LogF("game version: not found in the game's save functions (GetGameVersion returns 0)");
    }
    return found;
}

bool ResolveModList()
{
    bool ok = true;
    modList.instance = Symbols_Resolve("g_modsInstance");
    if (!modList.instance) {
        LogF("mod list: missing symbol g_modsInstance");
        ok = false;
    }

    auto field = [&](const char* type, const char* name) {
        int32_t offset = Symbols_FieldOffset(type, name);
        if (offset < 0) {
            LogF("mod list: missing field %s::%s", type, name);
            ok = false;
        }
        return offset;
    };
    modList.activeMods = field("Mods", "m_activeMods");
    modList.listData = field("List<StaticString<512> >", "m_list");
    modList.listCount = field("List<StaticString<512> >", "m_elements");
    modList.entryText = field("StaticString<512>", "m_szString");

    modList.entrySize = Symbols_TypeSize("StaticString<512>");
    if (modList.entrySize <= static_cast<uint32_t>(modList.entryText)) {
        LogF("mod list: missing type StaticString<512>");
        ok = false;
    }

    modList.ok = ok;
    return ok;
}

bool ActiveModPaths(std::vector<std::string>* out)
{
    out->clear();
    if (!modList.ok) {
        return false;
    }

    char* list = static_cast<char*>(modList.instance) + modList.activeMods;
    int32_t count = 0;
    const char* entries = nullptr;
    bool readList = ReadCode(reinterpret_cast<const uint8_t*>(list + modList.listCount), &count, sizeof(count)) &&
                    ReadCode(reinterpret_cast<const uint8_t*>(list + modList.listData), &entries, sizeof(entries));
    if (!readList) {
        return false;
    }
    if (count < 0 || count > 4096 || (count && !entries)) {
        return false; // not a list this build would have, so trust none of it
    }

    size_t textSize = std::min<size_t>(modList.entrySize - modList.entryText, 512); // the hash follows the text
    std::vector<char> text(textSize + 1, '\0');
    for (int32_t i = 0; i < count; ++i) {
        const char* entry = entries + static_cast<size_t>(i) * modList.entrySize + modList.entryText;
        if (!ReadCode(reinterpret_cast<const uint8_t*>(entry), text.data(), textSize)) {
            return false;
        }
        text[textSize] = '\0';
        out->emplace_back(text.data(), strnlen(text.data(), textSize));
    }
    return true;
}

} // namespace gameui
