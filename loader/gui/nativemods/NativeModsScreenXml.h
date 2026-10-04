// Builds the "Native mods" screen as game GUI XML (merged while the game loads its GUI, see NativeModsButton.cpp).
// No game names or hooks, so screentest checks it without the game.
#pragma once

#include <string>
#include <vector>

#include "dk2ml.h"

// Names and event codes shared by the XML and the runtime (NativeModsScreen.cpp).
namespace screenxml {

constexpr char kScreen[] = "Menu_NativeMods";
constexpr char kMainMenuButton[] = "#dk2ml_native_mods";
constexpr char kButtonIcon[] = "#dk2ml_nm_icon";             // the button's own icon, normal look
constexpr char kButtonIconHover[] = "#dk2ml_nm_icon_hover";  // and hovered
constexpr char kRestartBadge[] = "#dk2ml_nm_restart";        // on the button: a change needs a restart
constexpr char kBack[] = "#dk2ml_back";
constexpr char kNote[] = "#dk2ml_note";

// Widgets send <Action type="TriggerEvent" target="GUI_GAME_last" iParam="N"/>, event 219 (see GuiKit.cpp).
// N = kEventBase + widget * kCodesPerWidget + code; the screen opening sends kEventOpen.
constexpr int kEventBase = 0x4D4C0000;
constexpr int kEventOpen = kEventBase - 1;
constexpr int kCodesPerWidget = 8; // room for every EventCode

enum EventCode { kClick = 0, kCheckedOn = 1, kCheckedOff = 2, kPrev = 3, kNext = 4, kSliderMove = 5 };

enum class PageButton { WorkshopPage, ModFolder, SettingsFolder, OpenLog };

struct Option {
    DK2ML_OptionType type;
    std::string label;
    std::string tooltip;
    float min = 0;
    float max = 1;
    std::vector<std::string> choices;
};

struct Page {
    std::string title;
    std::vector<std::string> lines; // fixed info lines
    std::vector<std::string> warnLines; // fixed lines in the warning color, after the info lines
    bool hasStatus = false; // a status line the runtime fills in (ok / problem variants), right of the first line
    bool dimmed = false; // its entry in the list is muted (the mod isn't loaded)
    std::string tooltip; // on its entry in the list
    std::vector<PageButton> buttons;
    std::vector<Option> options;
};

struct Widget {
    enum class Kind { Select, Button, Option } kind;
    int page;
    int index; // Select: unused; Button: index into page.buttons; Option: index into page.options
};

struct Result {
    std::string xml; // the items, without the <GUIItems> wrapper
    std::vector<Widget> widgets; // widget N is named WidgetName(N)
};

Result Build(const std::vector<Page>& pages);

// The main-menu button that opens the screen (NativeModsButton.cpp moves it into Menu_Main > Extra Buttons).
// restart: with a badge and tooltip saying a restart is needed to apply native mod changes.
std::string MainMenuButton(bool restart);

std::string WidgetName(int n); // #dk2ml_w<n>
std::string ValueTextName(int n); // #dk2ml_w<n>_v: a slider's or selector's value text
std::string PageName(int page); // #dk2ml_page<i>
std::string StatusName(int page, bool ok); // #dk2ml_p<i>_ok / _bad
std::string OptionsListName(int page); // #dk2ml_opts<i>: the page's scrolling settings list
constexpr char kModList[] = "#dk2ml_list";

constexpr char kNoSettings[] = "This mod has no settings.";
constexpr char kSettingsWhenRunning[] = "Its settings, if it has any, show here while it runs."; // dimmed pages

std::string Escape(const std::string& modText); // XML attribute text; no leading '@' (game localization keys)
const char* PageButtonLabel(PageButton b);

} // namespace screenxml
