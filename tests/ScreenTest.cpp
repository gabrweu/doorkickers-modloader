// The generated "Native mods" screen XML (loader/gui/nativemods/NativeModsScreenXml.cpp) without the game:
// well-formed, mod text escaped, names unique, every widget reachable by its events.
#include <cstdio>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "../loader/gui/nativemods/NativeModsScreenXml.h"

namespace sx = screenxml;

namespace {

int g_failures = 0;

void Expect(const char* test, bool ok)
{
    printf("%-64s %s\n", test, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

// the entity starting at xml[at], up to its ';', is one of XML's five
bool KnownEntity(const std::string& xml, size_t at)
{
    size_t semi = xml.find(';', at);
    std::string ent = semi == std::string::npos ? "" : xml.substr(at, semi - at + 1);
    return ent == "&amp;" || ent == "&lt;" || ent == "&gt;" || ent == "&quot;" || ent == "&apos;";
}

// Minimal XML well-formedness: matching tags, quoted attributes, no raw '<' or '&' outside entities.
bool WellFormed(const std::string& xml, std::string* error)
{
    std::vector<std::string> stack;
    size_t i = 0;
    while (i < xml.size()) {
        if (xml[i] == '&') {
            if (!KnownEntity(xml, i)) {
                *error = "bad entity at " + std::to_string(i);
                return false;
            }
            i = xml.find(';', i) + 1;
            continue;
        }
        if (xml[i] != '<') {
            ++i;
            continue;
        }

        // find the tag's end, skipping '>' inside quoted attribute values
        size_t end = i + 1;
        char quote = 0;
        for (; end < xml.size(); ++end) {
            char c = xml[end];
            if (quote) {
                if (c == quote) {
                    quote = 0;
                } else if (c == '<') {
                    *error = "raw '<' inside an attribute at " + std::to_string(end);
                    return false;
                } else if (c == '&' && !KnownEntity(xml, end)) {
                    *error = "raw '&' inside an attribute at " + std::to_string(end);
                    return false;
                }
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>') {
                break;
            }
        }
        if (end >= xml.size()) {
            *error = "unterminated tag at " + std::to_string(i);
            return false;
        }

        std::string tag = xml.substr(i + 1, end - i - 1);
        if (tag.empty()) {
            *error = "empty tag";
            return false;
        }

        bool closing = tag[0] == '/';
        bool selfClosing = tag.back() == '/';
        std::string name = tag.substr(closing ? 1 : 0);
        name = name.substr(0, name.find_first_of(" /\t\r\n"));
        if (closing) {
            if (stack.empty() || stack.back() != name) {
                *error = "</" + name + "> doesn't match <" + (stack.empty() ? "" : stack.back()) + ">";
                return false;
            }
            stack.pop_back();
        } else if (!selfClosing) {
            stack.push_back(name);
        }
        i = end + 1;
    }

    if (!stack.empty()) {
        *error = "unclosed <" + stack.back() + ">";
        return false;
    }
    return true;
}

// the generated screen and button, with a count per item name
struct Screen {
    std::vector<sx::Page> pages;
    sx::Result r;
    std::string button;
    std::string plainButton;
    std::string doc;
    std::map<std::string, int> names;
};

std::vector<sx::Page> MakePages()
{
    std::vector<sx::Page> pages(3);
    pages[0].title = "Native Mod Loader";
    pages[0].lines = {"Loader 1.2.0"};
    pages[0].buttons = {sx::PageButton::OpenLog};

    pages[1].title = "@menu_m_sendfeedback <script>&\"evil\" 'mod'";
    pages[1].lines = {"Steam Workshop item 123 \xC2\xB7 a.dll"};
    pages[1].hasStatus = true;
    pages[1].buttons = {sx::PageButton::WorkshopPage, sx::PageButton::ModFolder};
    pages[1].options = {
        {DK2ML_OPTION_HEADER, "Camera", ""},
        {DK2ML_OPTION_BOOL, "Reverse <drag>", "tip & \"more\""},
        {DK2ML_OPTION_FLOAT, "Sensitivity", "", 0.1f, 5.0f},
        {DK2ML_OPTION_INT, "@keybind_toggle_play", "", 1, 10},
        {DK2ML_OPTION_CHOICE, "Mode", "", 0, 1, {"Slow", "@menu_generic_back", "Fast & loud"}},
        {DK2ML_OPTION_KEY, "Rotate left", ""},
        {DK2ML_OPTION_BUTTON, "Reset to defaults", ""},
    };

    pages[2].title = "Empty mod";
    pages[2].hasStatus = true;
    pages[2].buttons = {sx::PageButton::ModFolder};
    pages[2].dimmed = true;
    pages[2].tooltip = "Not loaded: you said No";
    return pages;
}

Screen BuildScreen()
{
    Screen s;
    s.pages = MakePages();
    s.r = sx::Build(s.pages);
    s.button = sx::MainMenuButton(true); // the restart variant, which adds the badge
    s.plainButton = sx::MainMenuButton(false);
    s.doc = "<GUIItems>" + s.button + s.r.xml + "</GUIItems>"; // as NativeModsButton.cpp merges it

    std::regex nameAttr("name=\"([^\"]*)\"");
    for (auto it = std::sregex_iterator(s.doc.begin(), s.doc.end(), nameAttr); it != std::sregex_iterator(); ++it) {
        ++s.names[(*it)[1].str()];
    }
    return s;
}

void TestWellFormed(const Screen& s)
{
    std::string error;
    bool wellFormed = WellFormed(s.doc, &error);
    if (!wellFormed) {
        printf("  %s\n", error.c_str());
    }
    Expect("generated XML is well-formed", wellFormed);

    // 3 select + 1 + 2 buttons + 1 button + 6 options (the header has none)
    Expect("one widget per page, button and non-header option", s.r.widgets.size() == 3 + 1 + 2 + 1 + 6);
}

void TestNames(const Screen& s)
{
    bool unique = true;
    for (auto& [name, count] : s.names) {
        if (count > 1) {
            printf("  name %s used %d times\n", name.c_str(), count);
            unique = false;
        }
    }
    Expect("item names are unique", unique);

    bool allWidgets = true;
    for (size_t n = 0; n < s.r.widgets.size(); ++n) {
        allWidgets &= s.names.count(sx::WidgetName(static_cast<int>(n))) == 1;
    }
    Expect("every widget has its named item", allWidgets);

    for (int p = 0; p < 3; ++p) {
        Expect(("page " + std::to_string(p) + " exists").c_str(), s.names.count(sx::PageName(p)) == 1);
    }

    bool statusWhereAsked = s.names.count(sx::StatusName(1, true)) && s.names.count(sx::StatusName(1, false)) &&
                            !s.names.count(sx::StatusName(0, true));
    Expect("status lines exist where asked", statusWhereAsked);
}

void TestEvents(const Screen& s)
{
    std::regex iparam("iParam=\"(-?[0-9]+)\"");
    std::set<int> events;
    for (auto it = std::sregex_iterator(s.doc.begin(), s.doc.end(), iparam); it != std::sregex_iterator(); ++it) {
        events.insert(std::stoi((*it)[1].str()));
    }

    bool allReachable = true;
    for (size_t n = 0; n < s.r.widgets.size(); ++n) {
        bool any = false;
        for (int code = 0; code < 8; ++code) {
            any |= events.count(sx::kEventBase + static_cast<int>(n) * 8 + code) > 0;
        }
        allReachable &= any;
    }
    Expect("every widget sends an event", allReachable);
    Expect("the screen sends its open event", events.count(sx::kEventOpen) == 1);
}

// mod text can't reach game localization: no attribute value of ours starts with '@' except the game's own Back
void TestModText(const Screen& s)
{
    std::regex textAttr("(text|tooltip)=\"(@[^\"]*)\"");
    bool noKeys = true;
    for (auto it = std::sregex_iterator(s.doc.begin(), s.doc.end(), textAttr); it != std::sregex_iterator(); ++it) {
        if ((*it)[2].str() != "@menu_generic_back") {
            printf("  %s\n", (*it)[0].str().c_str());
            noKeys = false;
        }
    }
    Expect("mod text never starts with '@'", noKeys);
    Expect("mod text is escaped",
           s.doc.find("&lt;script&gt;&amp;&quot;evil&quot; &apos;mod&apos;") != std::string::npos);
    Expect("escape strips control characters", sx::Escape(std::string("a\x01"
                                                                      "b")) == "a b");
}

// the mod list: radio entries, so the page on show is highlighted
void TestModList(const Screen& s)
{
    std::regex radio("<Checkbox name=\"[^\"]*\"[^>]*autoSiblingsUncheck=\"true\"");
    auto selectRows = std::distance(std::sregex_iterator(s.doc.begin(), s.doc.end(), radio), std::sregex_iterator());
    Expect("every mod-list entry is a radio checkbox", selectRows == 3);

    std::regex checkedOrange("<CheckedState acceptInput=\"false\"[^>]*><RenderObject2D[^>]*color=\"f97b03\"");
    Expect("the selected entry can't be clicked off", std::regex_search(s.doc, checkedOrange));

    bool mutedWithReason = s.doc.find("textColor=\"a08f80\"") != std::string::npos &&
                           s.doc.find("tooltip=\"Not loaded: you said No\"") != std::string::npos;
    Expect("a not-loaded mod is muted in the list and says why", mutedWithReason);
}

void TestPageHeader(const Screen& s)
{
    Expect("a not-loaded mod's page without options doesn't claim it has none",
           s.doc.find(sx::kSettingsWhenRunning) != std::string::npos &&
               s.doc.find(sx::kNoSettings) == std::string::npos);

    std::vector<sx::Page> running = s.pages;
    running[2].dimmed = false;
    Expect("a running mod's page without options says so",
           sx::Build(running).xml.find(sx::kNoSettings) != std::string::npos);

    Expect("the status sits right of the info line",
           s.doc.find("name=\"" + sx::StatusName(1, true) + "\" align=\"tr\"") != std::string::npos);
}

// the mouse wheel over a widget scrolls its list, which must exist
void TestWheel(const Screen& s)
{
    std::regex scroll("type=\"ScrollList(Forward|Backward)\" target=\"([^\"]*)\"");
    int scrolls = 0;
    bool scrollTargetsOk = true;
    for (auto it = std::sregex_iterator(s.doc.begin(), s.doc.end(), scroll); it != std::sregex_iterator(); ++it) {
        ++scrolls;
        std::string target = (*it)[2].str();
        if (target != sx::kModList && target != sx::OptionsListName(1)) {
            printf("  scrolls %s\n", target.c_str());
            scrollTargetsOk = false;
        }
    }

    bool listsExist = s.names.count(sx::kModList) == 1 && s.names.count(sx::OptionsListName(1)) == 1;
    Expect("wheel actions target the screen's lists", scrollTargetsOk && listsExist);
    // mod list: 3 entries; page 1: checkbox, 2 choice arrows, key, button (sliders keep the wheel)
    Expect("every list entry and non-slider widget passes the wheel on", scrolls == 2 * (3 + 5));
}

// Show/Hide targets must exist (resolved at load) and be the button's own icons or the two screens it switches between
void TestMainMenuButton(const Screen& s)
{
    const std::string& button = s.button;
    const std::string& plainButton = s.plainButton;

    std::string buttonError;
    Expect("main-menu button XML is well-formed", WellFormed(button, &buttonError));

    bool namedOnce = s.names.count(sx::kMainMenuButton) == 1 && s.names.count(sx::kButtonIcon) == 1 &&
                     s.names.count(sx::kButtonIconHover) == 1;
    Expect("main-menu button and its icons are named once", namedOnce);
    Expect("restart: the button has the badge and says why",
           s.names.count(sx::kRestartBadge) == 1 && button.find("Restart the game") != std::string::npos);

    // WellFormed first: it reuses buttonError
    bool plainOk = WellFormed(plainButton, &buttonError) && plainButton.find(sx::kRestartBadge) == std::string::npos &&
                   plainButton.find("Restart the game") == std::string::npos;
    Expect("no restart: no badge, the usual tooltip", plainOk);

    std::regex showHide("type=\"(Show|Hide)\" target=\"([^\"]*)\"");
    bool targetsOk = true;
    for (auto it = std::sregex_iterator(button.begin(), button.end(), showHide); it != std::sregex_iterator(); ++it) {
        std::string target = (*it)[2].str();
        bool ownIcon = target == sx::kButtonIcon || target == sx::kButtonIconHover;
        bool screen = target == "Menu_Main" || target == sx::kScreen;
        if (!ownIcon && !screen) {
            printf("  button targets %s\n", target.c_str());
            targetsOk = false;
        }
    }
    Expect("main-menu button only shows/hides its icons and the screens", targetsOk);

    Expect("main-menu button label is upper case like the game's",
           button.find("text=\"NATIVE MODS\"") != std::string::npos &&
               button.find("Native mods\"") == std::string::npos);
    Expect("main-menu button hover icon starts hidden",
           button.find(std::string("name=\"") + sx::kButtonIconHover + "\" origin=\"136 0\" hidden=\"true\"") !=
               std::string::npos);
}

void TestScreenTargets(const Screen& s)
{
    std::regex showHide("type=\"(Show|Hide)\" target=\"([^\"]*)\"");
    bool screenTargets = true;
    const std::string& screen = s.r.xml;
    for (auto it = std::sregex_iterator(screen.begin(), screen.end(), showHide); it != std::sregex_iterator(); ++it) {
        std::string target = (*it)[2].str();
        if (!s.names.count(target) && target != "Menu_Main") {
            printf("  screen targets %s\n", target.c_str());
            screenTargets = false;
        }
    }
    Expect("every Show/Hide in the screen names an existing item", screenTargets);
}

} // namespace

int main()
{
    Screen s = BuildScreen();

    TestWellFormed(s);
    TestNames(s);
    TestEvents(s);
    TestModText(s);
    TestModList(s);
    TestPageHeader(s);
    TestWheel(s);
    TestMainMenuButton(s);
    TestScreenTargets(s);

    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
