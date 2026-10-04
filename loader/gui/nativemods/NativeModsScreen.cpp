// The "Native mods" screen at runtime: its pages and the player's input.
//
// Clicks and changes arrive as event 219 in Screen_OnGameEvent (iParam: widget and action). Widgets are found by name
// (#dk2ml_w<N>) when the screen opens.
#include "NativeModsScreen.h"

#include <shellapi.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "gui/GameUi.h"
#include "NativeModsScreenXml.h"

using namespace gameui;
namespace sx = screenxml;

namespace {

struct PageInfo {
    int mod = -1;                       // index in Plugins_Mods(); -1 for the loader's own page
    std::vector<OptionEntry*> options;  // matches page.options
};

std::vector<sx::Page> g_pages;
std::vector<PageInfo> g_pageInfo;
std::vector<sx::Widget> g_widgets;
int g_selected = 0;

// found when the screen opens (a GUI reload rebuilds them)
void* g_screen = nullptr;
std::vector<void*> g_widgetItems;
std::vector<void*> g_valueTexts;
std::vector<void*> g_pageItems;
std::vector<void*> g_statusOk;
std::vector<void*> g_statusBad;
void* g_note = nullptr;

// key capture for a KEY option
int g_capturing = -1; // widget
bool g_keysHeldAtStart[256] = {};

std::string U8(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// No leading '@': ChangeText would look it up as a game text.
std::string Plain(const std::string& s)
{
    size_t i = 0;
    while (i < s.size() && s[i] == '@') {
        ++i;
    }
    return s.substr(i);
}

std::wstring SettingsFolder(const ModEntry& e)
{
    std::wstring save = Plugins_SaveDir();
    if (save.empty()) {
        return L"";
    }

    for (const auto& dll : e.dllNames) {
        std::wstring dir = save + L"dk2ml\\" + dll.substr(0, dll.find_last_of(L'.')) + L"\\";
        if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return dir;
        }
    }
    return L"";
}

constexpr char kRestartNote[] = "Restart the game to apply your native mod changes.";

std::string StatusText(const ModEntry& e)
{
    // disabled in the Mods menu, no code running
    if (!e.enabledNow && e.modules.empty()) {
        return "Not loaded: disabled in the Mods menu";
    }

    switch (e.status) {
    case ModStatus::TurnedOff:
        return e.enabledNow ? "Switched off when you disabled it: restart the game to run it again"
                            : "Switched off: you disabled it in the Mods menu (restart the game to unload it fully)";
    case ModStatus::EnabledLater:
        return e.source == ModSource::Workshop ? "Enabled after the game started: restart the game (it may ask first)"
                                               : "Enabled after the game started: restart the game to load it";
    case ModStatus::Duplicate: return "Not loaded: the same plugin is already loaded from " + U8(e.duplicateOf);
    case ModStatus::Loaded: return "Loaded";
    case ModStatus::Declined:
        return "Not loaded: you said No (asked again when it updates, or delete its line in dk2ml.ini)";
    case ModStatus::PathNotAllowed: return "Not loaded: not in your mods folder or the Workshop";
    case ModStatus::InitFailed: return "Failed to start (see dk2ml.log)";
    case ModStatus::Crashed: return "Crashed and was switched off for this session";
    case ModStatus::LoadFailed: return "Couldn't be loaded (see dk2ml.log)";
    case ModStatus::Unreadable: return "Not loaded: its files couldn't be read";
    case ModStatus::NeedsNewerLoader:
        return "Not loaded: needs a newer mod loader (plugin API " + std::to_string(e.neededApi) + ")";
    }
    return "?";
}

std::string ValueText(const OptionEntry& o, float v)
{
    char buf[128];
    bool integer = o.api.type == DK2ML_OPTION_INT;
    std::string f = Options_SafeFormat(o.format, integer);

    if (integer) {
        snprintf(buf, sizeof(buf), f.c_str(), static_cast<int>(std::lround(v)));
    } else {
        snprintf(buf, sizeof(buf), f.c_str(), v);
    }
    return Plain(buf);
}

std::string KeyName(int vk)
{
    switch (vk) { // mouse buttons have no scan code, so GetKeyNameText can't name them
    case VK_MBUTTON: return "Middle mouse";
    case VK_XBUTTON1: return "Mouse 4 (back)";
    case VK_XBUTTON2: return "Mouse 5 (forward)";
    }

    UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lParam = static_cast<LONG>(scan) << 16;
    switch (vk) { // extended keys need bit 24 for the right name
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_DIVIDE:
    case VK_NUMLOCK:
    case VK_RMENU:
    case VK_RCONTROL: lParam |= 1 << 24; break;
    }

    char name[64] = {};
    if (scan && GetKeyNameTextA(lParam, name, sizeof(name)) > 0) {
        return name;
    }
    snprintf(name, sizeof(name), "Key 0x%02X", vk);
    return name;
}

void SetText(void* text, const std::string& s)
{
    if (text) {
        fn.StaticTextChangeText(text, s.c_str());
    }
}

void SetButtonText(void* button, const std::string& s)
{
    if (!button) {
        return;
    }
    void** texts = &Field<void*>(button, off.buttonTexts);
    for (int i = 0; i < 3; ++i) {
        SetText(texts[i], s);
    }
}

void Show(void* item, bool visible)
{
    if (!item) {
        return;
    }
    if (visible) {
        fn.ItemShow(item);
    } else {
        fn.ItemHide(item);
    }
}

// No destructors in here (__try).
bool CallOnChange(OptionEntry* o)
{
    __try {
        if (o->api.onChange) {
            o->api.onChange(&o->api, o->api.user);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RefreshStatus(int page);

void NotifyChange(OptionEntry* o, int page)
{
    if (o->faulted) {
        return;
    }
    if (!CallOnChange(o)) {
        Plugins_OnOptionCrash(o->owner);
        RefreshStatus(page);
    }
}

std::string ModTitleOf(HMODULE owner)
{
    for (const ModEntry& e : Plugins_Mods()) {
        for (HMODULE m : e.modules) {
            if (m == owner) {
                return U8(e.title);
            }
        }
    }
    return "another mod";
}

// other is another plugin's running KEY option, bound to vk
bool ClashesWith(const OptionEntry* other, const OptionEntry* o, int vk)
{
    if (other == o || other->owner == o->owner || other->faulted) {
        return false;
    }
    return other->api.type == DK2ML_OPTION_KEY && other->api.value && *static_cast<int*>(other->api.value) == vk;
}

// Other plugins' KEY options on the same key; the button names their mods.
std::string KeyClash(const OptionEntry* o)
{
    int vk = *static_cast<int*>(o->api.value);
    std::string others;
    for (const OptionEntry* other : Plugins_Options()) {
        if (!ClashesWith(other, o, vk)) {
            continue;
        }
        std::string title = ModTitleOf(other->owner);
        if (others.find(title) != std::string::npos) {
            continue;
        }
        if (!others.empty()) {
            others += ", ";
        }
        others += title;
    }
    return others;
}

// A KEY option's button text: the key and other mods bound to it.
std::string KeyText(const OptionEntry* o)
{
    std::string clash = KeyClash(o);
    std::string text = KeyName(*static_cast<int*>(o->api.value));
    if (!clash.empty()) {
        text += " (also " + Plain(clash) + ")";
    }
    return text;
}

// --- reading values into widgets ---

void SyncOption(int n)
{
    const sx::Widget& w = g_widgets[n];
    OptionEntry* o = g_pageInfo[w.page].options[w.index];
    void* item = g_widgetItems[n];
    if (!item || !o->api.value) {
        return;
    }

    switch (o->api.type) {
    case DK2ML_OPTION_BOOL: fn.CheckboxSetState(item, *static_cast<bool*>(o->api.value) ? 1 : 0, false); break;
    case DK2ML_OPTION_FLOAT: {
        float v = *static_cast<float*>(o->api.value);
        fn.SliderSetValue(item, v);
        SetText(g_valueTexts[n], ValueText(*o, v));
        break;
    }
    case DK2ML_OPTION_INT: {
        float v = static_cast<float>(*static_cast<int*>(o->api.value));
        fn.SliderSetValue(item, v);
        SetText(g_valueTexts[n], ValueText(*o, v));
        break;
    }
    case DK2ML_OPTION_CHOICE: {
        int i = *static_cast<int*>(o->api.value);
        int count = static_cast<int>(o->choices.size());
        bool known = i >= 0 && i < count;
        SetText(g_valueTexts[n], known ? Plain(o->choices[i]) : "?");
        break;
    }
    case DK2ML_OPTION_KEY: SetButtonText(item, KeyText(o)); break;
    default: break;
    }
}

bool IsKeyOption(const sx::Widget& w)
{
    return w.kind == sx::Widget::Kind::Option && g_pageInfo[w.page].options[w.index]->api.type == DK2ML_OPTION_KEY;
}

// a key change can make or end a clash on any page
void SyncKeyOptions()
{
    for (size_t n = 0; n < g_widgets.size() && n < g_widgetItems.size(); ++n) {
        if (IsKeyOption(g_widgets[n])) {
            SyncOption(static_cast<int>(n));
        }
    }
}

void SyncPageOptions(int page)
{
    for (size_t n = 0; n < g_widgets.size(); ++n) {
        if (g_widgets[n].kind == sx::Widget::Kind::Option && g_widgets[n].page == page) {
            SyncOption(static_cast<int>(n));
        }
    }
}

void RefreshStatus(int page)
{
    const PageInfo& info = g_pageInfo[page];
    if (info.mod < 0 || page >= static_cast<int>(g_statusOk.size())) {
        return;
    }

    const ModEntry& e = Plugins_Mods()[info.mod];
    bool ok = e.status == ModStatus::Loaded;
    Show(g_statusOk[page], ok);
    Show(g_statusBad[page], !ok);
    SetText(ok ? g_statusOk[page] : g_statusBad[page], Plain(StatusText(e)));
}

// clicked: the clicked list entry, or -1. Its click event runs before it flips, so setting it here would be flipped
// back: it's left alone. The others are unchecked in case autoSiblingsUncheck missed one.
void ShowPage(int page, int clicked = -1)
{
    g_selected = page;
    for (size_t p = 0; p < g_pageItems.size(); ++p) {
        Show(g_pageItems[p], static_cast<int>(p) == page);
    }

    for (size_t n = 0; n < g_widgets.size() && n < g_widgetItems.size(); ++n) {
        const sx::Widget& w = g_widgets[n];
        if (w.kind == sx::Widget::Kind::Select && g_widgetItems[n] && static_cast<int>(n) != clicked) {
            fn.CheckboxSetState(g_widgetItems[n], w.page == page ? 1 : 0, false);
        }
    }
}

// Logs OnOpen above 100 ms.
struct OpenTimer {
    ULONGLONG started;

    ~OpenTimer()
    {
        ULONGLONG ms = GetTickCount64() - started;
        if (ms > 100) {
            LogF("Native mods screen: opened in %llu ms", static_cast<unsigned long long>(ms));
        }
    }
};

void OnOpen()
{
    // Main thread: anything slow here freezes the game before the screen shows.
    OpenTimer timer = {GetTickCount64()};

    void* manager = *globals.guiManager;
    void* root = manager ? Field<void*>(manager, off.guiRoot) : nullptr;
    g_screen = root ? fn.FindChild(root, sx::kScreen) : nullptr;
    if (!g_screen) {
        return;
    }

    g_widgetItems.assign(g_widgets.size(), nullptr);
    g_valueTexts.assign(g_widgets.size(), nullptr);
    for (size_t n = 0; n < g_widgets.size(); ++n) {
        g_widgetItems[n] = fn.FindChild(g_screen, sx::WidgetName(static_cast<int>(n)).c_str());
        g_valueTexts[n] = fn.FindChild(g_screen, sx::ValueTextName(static_cast<int>(n)).c_str());
    }

    g_pageItems.assign(g_pages.size(), nullptr);
    g_statusOk.assign(g_pages.size(), nullptr);
    g_statusBad.assign(g_pages.size(), nullptr);
    for (size_t p = 0; p < g_pages.size(); ++p) {
        int pi = static_cast<int>(p);
        g_pageItems[p] = fn.FindChild(g_screen, sx::PageName(pi).c_str());
        g_statusOk[p] = fn.FindChild(g_screen, sx::StatusName(pi, true).c_str());
        g_statusBad[p] = fn.FindChild(g_screen, sx::StatusName(pi, false).c_str());
        SyncPageOptions(pi);
        RefreshStatus(pi);
    }

    g_note = fn.FindChild(g_screen, sx::kNote);
    SetText(g_note, Plugins_RestartNeeded() ? kRestartNote : "");
    g_capturing = -1;
    ShowPage(g_selected < static_cast<int>(g_pages.size()) ? g_selected : 0);
}

void Open(const std::wstring& target)
{
    // only loader-known paths and the fixed Workshop URL with a numeric id
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OnPageButton(const sx::Widget& w, int n)
{
    sx::PageButton b = g_pages[w.page].buttons[w.index];
    std::wstring dir = Plugins_Ini().substr(0, Plugins_Ini().find_last_of(L'\\') + 1);
    if (b == sx::PageButton::OpenLog) {
        Open(dir + L"dk2ml.log");
        return;
    }

    const PageInfo& info = g_pageInfo[w.page];
    if (info.mod < 0) {
        return;
    }

    ModEntry& e = Plugins_Mods()[info.mod];
    switch (b) {
    case sx::PageButton::WorkshopPage:
        Open(L"https://steamcommunity.com/sharedfiles/filedetails/?id=" + e.workshopId);
        break;
    case sx::PageButton::ModFolder: Open(e.modDir); break;
    case sx::PageButton::SettingsFolder: {
        std::wstring settings = SettingsFolder(e);
        if (!settings.empty()) {
            Open(settings);
        }
        break;
    }
    default: break;
    }
    (void)n;
}

void StartCapture(int n)
{
    g_capturing = n;
    for (int vk = 0; vk < 256; ++vk) {
        g_keysHeldAtStart[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    SetButtonText(g_widgetItems[n], "Press a key (Esc cancels)");
}

// Stores the value (INT: rounded) if it changed.
void OnSliderMove(int n, OptionEntry* o, int page, void* item)
{
    float v = fn.SliderGetValue(item);
    if (o->api.type == DK2ML_OPTION_INT) {
        int i = static_cast<int>(std::lround(v));
        if (*static_cast<int*>(o->api.value) == i) {
            return;
        }
        *static_cast<int*>(o->api.value) = i;
    } else {
        if (*static_cast<float*>(o->api.value) == v) {
            return;
        }
        *static_cast<float*>(o->api.value) = v;
    }

    SetText(g_valueTexts[n], ValueText(*o, v));
    NotifyChange(o, page);
}

// Next/previous choice, wrapping; an out-of-range value counts as the first.
void StepChoice(OptionEntry* o, bool forward)
{
    int count = static_cast<int>(o->choices.size());
    int& i = *static_cast<int*>(o->api.value);
    int current = i < 0 || i >= count ? 0 : i;
    int step = forward ? 1 : count - 1;
    i = (current + step) % count;
}

void OnOptionEvent(int n, int code)
{
    const sx::Widget& w = g_widgets[n];
    OptionEntry* o = g_pageInfo[w.page].options[w.index];
    if (o->faulted) {
        return;
    }

    void* item = g_widgetItems[n];
    switch (o->api.type) {
    case DK2ML_OPTION_BOOL:
        if (code == sx::kCheckedOn || code == sx::kCheckedOff) {
            *static_cast<bool*>(o->api.value) = code == sx::kCheckedOn; // the checkbox flips right after this
            NotifyChange(o, w.page);
        }
        break;
    case DK2ML_OPTION_FLOAT:
    case DK2ML_OPTION_INT:
        if (code == sx::kSliderMove && item) {
            OnSliderMove(n, o, w.page, item);
        }
        break;
    case DK2ML_OPTION_CHOICE:
        if (code == sx::kPrev || code == sx::kNext) {
            StepChoice(o, code == sx::kNext);
            SyncOption(n);
            NotifyChange(o, w.page);
        }
        break;
    case DK2ML_OPTION_KEY:
        if (code == sx::kClick) {
            StartCapture(n);
        }
        break;
    case DK2ML_OPTION_BUTTON:
        if (code == sx::kClick) {
            NotifyChange(o, w.page);
            SyncPageOptions(w.page); // e.g. "reset to defaults" changed the values
        }
        break;
    default: break;
    }
}

void HandleEvent(int param)
{
    if (param == sx::kEventOpen) {
        OnOpen();
        return;
    }

    int rel = param - sx::kEventBase;
    if (rel < 0 || !g_screen) {
        return;
    }

    int n = rel / sx::kCodesPerWidget;
    int code = rel % sx::kCodesPerWidget;
    if (n >= static_cast<int>(g_widgets.size())) {
        return;
    }

    const sx::Widget& w = g_widgets[n];
    switch (w.kind) {
    case sx::Widget::Kind::Select:
        if (code == sx::kClick) {
            ShowPage(w.page, n);
        }
        break;
    case sx::Widget::Kind::Button:
        if (code == sx::kClick) {
            OnPageButton(w, n);
        }
        break;
    case sx::Widget::Kind::Option: OnOptionEvent(n, code); break;
    }
}

bool CallHandleEvent(int param)
{
    __try {
        HandleEvent(param);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GameFocused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

// Left/right Shift, Ctrl, Alt are stored as the plain key, which either side presses.
int GenericModifier(int vk)
{
    switch (vk) {
    case VK_LSHIFT:
    case VK_RSHIFT: return VK_SHIFT;
    case VK_LCONTROL:
    case VK_RCONTROL: return VK_CONTROL;
    case VK_LMENU:
    case VK_RMENU: return VK_MENU;
    default: return vk;
    }
}

// --- building the pages ---

std::wstring JoinNames(const std::vector<std::wstring>& names)
{
    std::wstring joined;
    for (const auto& name : names) {
        if (!joined.empty()) {
            joined += L", ";
        }
        joined += name;
    }
    return joined;
}

sx::Page LoaderPage()
{
    sx::Page loader;
    loader.title = "Native Mod Loader";
    loader.lines.push_back(std::string("Door Kickers 2 Native Mod Loader ") + DK2ML_VERSION + ", plugin API v" +
                           std::to_string(DK2ML_API_VERSION));
    loader.lines.push_back("Runs mods that contain code (DLLs). Workshop mods only run with your permission, asked "
                           "again when they change.");
    loader.lines.push_back(
        "If a native mod keeps the game from starting, set enabled=0 in dk2ml.ini in the game folder.");

    if (Plugins_Mods().empty()) {
        loader.lines.push_back("None of your enabled mods contain native code.");
    }
    if (Plugins_ModListFromFile()) {
        loader.lines.push_back(
            "Mods menu changes are read from options.xml (this game version's mod list wasn't found).");
    }
    loader.buttons.push_back(sx::PageButton::OpenLog);
    return loader;
}

// Where it's from, then its code (the status sits to its right).
std::string SourceLine(const ModEntry& e)
{
    std::string from;
    if (e.gameNativeFolder) {
        from = "Always loaded from the game folder";
    } else if (e.source == ModSource::Workshop) {
        from = "Steam Workshop item " + U8(e.workshopId);
    } else if (e.source == ModSource::Local) {
        from = "Your mods folder";
    } else {
        from = "Unexpected folder: " + U8(e.modDir);
    }

    std::wstring code = JoinNames(e.dllNames);
    std::wstring support = JoinNames(e.supportNames);
    std::string line = from + "  \xC2\xB7  " + U8(code);
    if (!support.empty()) {
        line += " (uses " + U8(support) + ")";
    }
    return line;
}

void AddManifestLines(const ModEntry& e, sx::Page& page)
{
    for (size_t k = 0; k < e.manifests.size() && k < e.dllNames.size(); ++k) {
        const PluginManifest& m = e.manifests[k];
        if (!m.present) {
            continue;
        }
        std::string line = Consent_ManifestLine(m);
        if (e.dllNames.size() > 1) {
            line = U8(e.dllNames[k]) + ": " + line;
        }
        if (m.gameVersion) {
            line += "  \xC2\xB7  made for game version " + std::to_string(m.gameVersion);
        }
        if (!m.url.empty()) {
            line += "  \xC2\xB7  " + m.url;
        }
        page.lines.push_back(line);
    }
}

// The first three missing names, and how many more the log has.
std::string MissingNames(const ModEntry& e)
{
    std::string list;
    for (size_t k = 0; k < e.missing.size() && k < 3; ++k) {
        if (k) {
            list += ", ";
        }
        list += e.missing[k];
    }
    if (e.missing.size() > 3) {
        list += " (+" + std::to_string(e.missing.size() - 3) + " more in dk2ml.log)";
    }
    return list;
}

// Declared settings, unless switched off.
void AddModOptions(const ModEntry& e, sx::Page& page, PageInfo& info)
{
    for (OptionEntry* o : Plugins_Options()) {
        bool mine = false;
        for (HMODULE m : e.modules) {
            mine |= o->owner == m;
        }
        if (!mine || o->faulted) {
            continue;
        }

        sx::Option opt;
        opt.type = o->api.type;
        opt.label = o->label;
        opt.tooltip = o->tooltip;
        opt.min = o->api.min;
        opt.max = o->api.max;
        opt.choices = o->choices;
        page.options.push_back(opt);
        info.options.push_back(o);
    }
}

sx::Page ModPage(const ModEntry& e, PageInfo& info)
{
    sx::Page page;
    page.title = U8(e.title);
    page.lines.push_back(SourceLine(e));
    AddManifestLines(e, page);

    // after a game update, the usual reason a plugin didn't start; players pass these on
    if (e.status != ModStatus::Loaded && !e.missing.empty()) {
        page.warnLines.push_back("Not in this game version: " + MissingNames(e));
    }
    // clashes with other mods (shared DLL names)
    for (const auto& c : e.conflicts) {
        page.warnLines.push_back(c);
    }

    page.hasStatus = true;
    // Not running: dimmed, reason as tooltip. Set at GUI load: a later crash shows on the status line first.
    page.dimmed = e.status != ModStatus::Loaded;
    if (page.dimmed) {
        page.tooltip = StatusText(e);
    }

    if (e.source == ModSource::Workshop) {
        page.buttons.push_back(sx::PageButton::WorkshopPage);
    }
    page.buttons.push_back(sx::PageButton::ModFolder);
    if (!SettingsFolder(e).empty()) {
        page.buttons.push_back(sx::PageButton::SettingsFolder);
    }

    AddModOptions(e, page, info);
    return page;
}

} // namespace

std::string Screen_BuildItems()
{
    g_pages.clear();
    g_pageInfo.clear();
    g_screen = nullptr;

    g_pages.push_back(LoaderPage());
    g_pageInfo.push_back({});

    auto& mods = Plugins_Mods();
    for (size_t i = 0; i < mods.size(); ++i) {
        PageInfo info;
        info.mod = static_cast<int>(i);
        g_pages.push_back(ModPage(mods[i], info));
        g_pageInfo.push_back(info);
    }

    sx::Result r = sx::Build(g_pages);
    g_widgets = r.widgets;
    return r.xml;
}

void Screen_OnGuiLoaded()
{
    g_screen = nullptr; // found again when it opens
}

void Screen_OnGameEvent(void* params)
{
    if (!params) {
        return;
    }
    int param = Field<int>(params, off.eventParamsInt);
    int widgetEventsEnd = sx::kEventBase + sx::kCodesPerWidget * static_cast<int>(g_widgets.size());
    bool isOpen = param == sx::kEventOpen;
    bool isWidget = param >= sx::kEventBase && param < widgetEventsEnd;
    if (!isOpen && !isWidget) {
        return; // someone else's use of the event
    }

    if (!CallHandleEvent(param)) {
        LogF("menu: crashed handling a Native mods screen event (%d)", param);
    }
}

void Screen_Tick()
{
    if (g_capturing < 0) {
        return;
    }
    if (!g_screen || Field<bool>(g_screen, off.itemHidden)) {
        g_capturing = -1;
        return;
    }
    if (!GameFocused()) {
        return;
    }

    // Mouse buttons count, except left/right (the capturing click; the game's main buttons): start at VK_MBUTTON (4),
    // past VK_LBUTTON/VK_RBUTTON/VK_CANCEL; skip unassigned 0x07.
    for (int vk = VK_MBUTTON; vk < 0xFF; ++vk) {
        if (vk == 0x07) {
            continue;
        }
        bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (!down) {
            g_keysHeldAtStart[vk] = false;
            continue;
        }
        // plain Shift/Ctrl/Alt are down along with their left/right codes, which come later
        bool plainModifier = vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU;
        if (g_keysHeldAtStart[vk] || plainModifier) {
            continue;
        }

        int n = g_capturing;
        g_capturing = -1;
        const sx::Widget& w = g_widgets[n];
        OptionEntry* o = g_pageInfo[w.page].options[w.index];
        if (vk != VK_ESCAPE && !o->faulted) {
            *static_cast<int*>(o->api.value) = GenericModifier(vk);
            NotifyChange(o, w.page);
        }
        SyncKeyOptions();
        return;
    }
}
