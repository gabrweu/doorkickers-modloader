#include "NativeModsScreenXml.h"

#include <sstream>

// Styles copied from options.xml/mods.xml: canvas 2560x1440, (0,0) at the center, y up; "align" anchors to that side
// of the parent, "origin" offsets from there. Palette: orange f97b03, cream f0e3cc, dark text 201f1b, panels
// 211e1dB3, row stripes 00000050, button body 40392b (also Back's pressed tint).

namespace screenxml {

namespace {

constexpr int kRowHeight = 60;
constexpr int kPanelHeight = 1060;
constexpr int kListWidth = 560;
constexpr int kPageWidth = 1380;
constexpr int kWidgetX = 260;   // option widgets, from the row's center
constexpr int kValueX = 560;    // their value text
constexpr const char* kTex = "data/textures/gui/";

std::string Num(float v)
{
    std::ostringstream s;
    s << v;
    return s.str();
}

std::string Event(int n, int code)
{
    return "<Action type=\"TriggerEvent\" target=\"GUI_GAME_last\" iParam=\"" +
           std::to_string(kEventBase + n * kCodesPerWidget + code) + "\"/>";
}

std::string Tooltip(const std::string& text)
{
    return text.empty() ? "" : " tooltip=\"" + Escape(text) + "\"";
}

std::string Square(int w, int h, const char* color)
{
    return std::string("<RenderObject2D texture=\"") + kTex + "square.tga\" sizeX=\"" + std::to_string(w) +
           "\" sizeY=\"" + std::to_string(h) + "\" color=\"" + color + "\"/>";
}

// The wheel over an item that takes input doesn't reach its list; the game's list templates forward it like this.
std::string Wheel(const std::string& list)
{
    // clang-format off
    return "<OnScrollDown><Action type=\"ScrollListForward\" target=\"" + list + "\"/></OnScrollDown>"
           "<OnScrollUp><Action type=\"ScrollListBackward\" target=\"" + list + "\"/></OnScrollUp>";
    // clang-format on
}

int AddWidget(std::vector<Widget>& widgets, Widget::Kind kind, int page, int index)
{
    int n = static_cast<int>(widgets.size());
    widgets.push_back({kind, page, index});
    return n;
}

// A flat button: dark, cream on hover, orange when pressed. inner: extra child elements.
std::string FlatButton(const std::string& name, const std::string& align, int x, int y, int w, int h,
                       const std::string& text, const char* font, const std::string& actions,
                       const std::string& tooltip = "", const std::string& inner = "")
{
    auto label = [&](const char* color) {
        return "<ButtonText align=\"c\" origin=\"0 0\" sizeX=\"" + std::to_string(w - 20) + "\" text=\"" +
               Escape(text) + "\" font=\"" + font + "\" fontAutoDownsize=\"true\" textColor=\"" + color + "\"/>";
    };
    std::string alignAttr = align.empty() ? "" : " align=\"" + align + "\"";

    std::ostringstream s;
    // clang-format off
    s << "<Button name=\"" << name << "\"" << alignAttr << " origin=\"" << x << " " << y << "\" stealFocus=\"true\""
      << Tooltip(tooltip) << ">"
          << inner
          << Square(w, h, "40392b") << label("f0e3cc")
          << "<OnHover>" << Square(w, h, "f0e3cc") << label("201f1b") << "</OnHover>"
          << "<OnClick>" << Square(w, h, "f97b03") << label("201f1b") << actions << "</OnClick>"
      << "</Button>";
    // clang-format on
    return s.str();
}

std::string ScrollBar()
{
    // clang-format off
    return std::string("<ButtonGraphics align=\"r\" origin=\"32 0\">") +
               Square(32, 32, "f97b0340") +
               "<OnHover>" + Square(32, 32, "f97b03ff") + "</OnHover>" +
               "<OnClick>" + Square(32, 32, "f97b03ff") + "</OnClick>" +
           "</ButtonGraphics>";
    // clang-format on
}

std::string Text(const std::string& name, const std::string& align, int x, int y, const std::string& text,
                 const char* font, const char* color, const std::string& extra = "")
{
    std::ostringstream s;
    s << "<StaticText";
    if (!name.empty()) {
        s << " name=\"" << name << "\"";
    }
    s << " align=\"" << align << "\" origin=\"" << x << " " << y << "\"";
    s << " text=\"" << Escape(text) << "\" font=\"" << font << "\" textColor=\"" << color << "\"";
    s << extra << "/>";
    return s.str();
}

// The Options panels' 72px orange header strip.
std::string PanelHeader(int width, const std::string& title)
{
    // clang-format off
    return "<StaticImage align=\"t\" origin=\"0 0\">" + Square(width, 72, "f97b03") +
               "<StaticImage origin=\"0 0\" align=\"rt\">" +
                   "<RenderObject2D texture=\"" + std::string(kTex) + "deploy/deploy_class_diagonalbars.dds\""
                   " flipX=\"true\" sizeX=\"140\" sizeY=\"75\" color=\"0c0b0b33\"/>" +
               "</StaticImage>" +
               Text("", "l", 20, 0, title, "header_3", "201f1b") +
           "</StaticImage>";
    // clang-format on
}

// A mod-list entry: a radio checkbox like globals.xml #squad_item_template. autoSiblingsUncheck unchecks the others;
// the checked state takes no input, so it can't be clicked off. Checked (orange) = the page on show.
std::string SelectRow(int n, int y, const Page& page)
{
    const int w = kListWidth - 40, h = 60;
    auto label = [&](const char* color) {
        return "<ButtonText align=\"c\" origin=\"0 0\" sizeX=\"" + std::to_string(w - 110) + "\" text=\"" +
               Escape(page.title) + "\" font=\"header_4\" fontAutoDownsize=\"true\" textColor=\"" + color + "\"/>";
    };
    std::string size = " sizeX=\"" + std::to_string(w) + "\" sizeY=\"" + std::to_string(h) + "\"";
    const char* textColor = page.dimmed ? "a08f80" : "f0e3cc";

    std::ostringstream s;
    // clang-format off
    s << "<Checkbox name=\"" << WidgetName(n) << "\" origin=\"0 " << y << "\"" << size
      << " stealFocus=\"true\" autoSiblingsUncheck=\"true\" defaultState=\"UncheckedState\""
      << Tooltip(page.tooltip) << ">"
          << Wheel(kModList)
          << "<UncheckedState" << size << ">"
              << Square(w, h, "40392b") << label(textColor)
              << "<OnHover>" << Square(w, h, "f0e3cc") << label("201f1b") << "</OnHover>"
              << "<OnClick>" << Event(n, kClick) << "</OnClick>"
          << "</UncheckedState>"
          << "<CheckedState acceptInput=\"false\"" << size << ">"
              << Square(w, h, "f97b03") << label("201f1b")
          << "</CheckedState>"
      << "</Checkbox>";
    // clang-format on
    return s.str();
}

std::string OptionWidget(const Option& o, int n, const std::string& list)
{
    std::string name = WidgetName(n);
    std::string wheel = Wheel(list);
    std::ostringstream s;

    switch (o.type) {
    case DK2ML_OPTION_BOOL: {
        auto state = [&](const char* element, const char* tex, const char* hoverTex, int code) {
            // clang-format off
            return std::string("<") + element + Tooltip(o.tooltip) + ">" +
                       "<RenderObject2D texture=\"" + kTex + tex + "\"/>" +
                       "<OnHover><RenderObject2D texture=\"" + kTex + hoverTex + "\"/></OnHover>" +
                       "<OnClick>" + Event(n, code) + "</OnClick>" +
                   "</" + element + ">";
            // clang-format on
        };

        // a click runs the current state's OnClick, before the flip
        // clang-format off
        s << "<Checkbox name=\"" << name << "\" origin=\"" << kWidgetX << " 0\""
          << " stealFocus=\"true\" defaultState=\"UncheckedState\">"
              << wheel
              << state("UncheckedState", "menu_checkbox_empty.tga", "menu_checkbox_empty_hover.tga", kCheckedOn)
              << state("CheckedState", "menu_checkbox_checked.tga", "menu_checkbox_checked_hover.tga", kCheckedOff)
          << "</Checkbox>";
        // clang-format on
        break;
    }

    case DK2ML_OPTION_FLOAT:
    case DK2ML_OPTION_INT: {
        const char* integerValues = o.type == DK2ML_OPTION_INT ? "true" : "false";
        // clang-format off
        s << "<Slider name=\"" << name << "\" origin=\"" << kWidgetX << " 0\" stealFocus=\"true\" sliderType=\"linear\""
          << " startValue=\"" << Num(o.min) << "\" endValue=\"" << Num(o.max) << "\""
          << " integerValues=\"" << integerValues << "\">"
              << "<BackgroundGraphics>"
                  << "<RenderObject2D texture=\"" << kTex << "menu_sliderbar_wide.tga\"/>"
              << "</BackgroundGraphics>"
              << "<ButtonGraphics>"
                  << "<RenderObject2D texture=\"" << kTex << "menu_sliderpointer_normal.tga\"/>"
                  << "<OnHover><RenderObject2D texture=\"" << kTex << "menu_sliderpointer_hover.tga\"/></OnHover>"
                  << "<OnClick><RenderObject2D texture=\"" << kTex << "menu_sliderpointer_hover.tga\"/></OnClick>"
              << "</ButtonGraphics>"
              << "<OnCursorMove>" << Event(n, kSliderMove) << "</OnCursorMove>"
          << "</Slider>"
          << Text(ValueTextName(n), "c", kValueX, 0, "", "paragraph_2", "f0e3cc");
        // clang-format on
        break;
    }

    case DK2ML_OPTION_CHOICE: {
        auto arrow = [&](const char* align, bool flip, int code) {
            std::string f = flip ? " flipX=\"true\"" : "";
            // clang-format off
            return std::string("<Button origin=\"0 0\" align=\"") + align + "\" stealFocus=\"true\">" +
                       wheel +
                       "<RenderObject2D texture=\"" + kTex + "menu_arrowright_normal.tga\"" + f + "/>" +
                       "<OnHover>" +
                           "<RenderObject2D texture=\"" + kTex + "menu_arrowright_hover.tga\"" + f + "/>" +
                       "</OnHover>" +
                       "<OnClick>" +
                           "<RenderObject2D texture=\"" + kTex + "menu_arrowright_normal.tga\"" + f + "/>" +
                           Event(n, code) +
                       "</OnClick>" +
                   "</Button>";
            // clang-format on
        };

        // clang-format off
        s << "<Item name=\"" << name << "\" origin=\"" << kWidgetX << " 0\">"
              << "<StaticImage" << Tooltip(o.tooltip) << ">"
                  << "<RenderObject2D texture=\"" << kTex << "menu_class_background.tga\"/>"
                  << Text(ValueTextName(n), "c", 0, 0, "", "paragraph_2", "f0e3cc",
                          " sizeX=\"220\" fontAutoDownsize=\"true\"")
                  << arrow("l", true, kPrev)
                  << arrow("r", false, kNext)
              << "</StaticImage>"
          << "</Item>";
        // clang-format on
        break;
    }

    case DK2ML_OPTION_KEY: {
        auto label = [&](const char* color) {
            // clang-format off
            return std::string("<ButtonText align=\"c\" origin=\"0 0\" sizeX=\"220\" text=\"-\" font=\"paragraph_2\""
                               " fontAutoDownsize=\"true\" textColor=\"") + color + "\"/>";
            // clang-format on
        };

        // clang-format off
        s << "<Button name=\"" << name << "\" origin=\"" << kWidgetX << " 0\" stealFocus=\"true\""
          << Tooltip(o.tooltip) << ">"
              << wheel
              << "<RenderObject2D texture=\"" << kTex << "menu_class_background.tga\"/>" << label("a08f80")
              << "<OnHover>" << label("f0e3cc") << "</OnHover>"
              << "<OnClick>" << label("f97b03") << Event(n, kClick) << "</OnClick>"
          << "</Button>";
        // clang-format on
        break;
    }

    case DK2ML_OPTION_BUTTON:
        s << FlatButton(name, "", kWidgetX, 0, 300, 48, o.label, "header_4", Event(n, kClick), o.tooltip, wheel);
        break;

    default: break;
    }
    return s.str();
}

} // namespace

std::string WidgetName(int n)
{
    return "#dk2ml_w" + std::to_string(n);
}

std::string ValueTextName(int n)
{
    return WidgetName(n) + "_v";
}

std::string PageName(int page)
{
    return "#dk2ml_page" + std::to_string(page);
}

std::string StatusName(int page, bool ok)
{
    return "#dk2ml_p" + std::to_string(page) + (ok ? "_ok" : "_bad");
}

std::string OptionsListName(int page)
{
    return "#dk2ml_opts" + std::to_string(page);
}

std::string Escape(const std::string& modText)
{
    std::string out;
    size_t start = 0;
    while (start < modText.size() && modText[start] == '@') {
        ++start;
    }

    for (size_t i = start; i < modText.size(); ++i) {
        char c = modText[i];
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:
            if (static_cast<unsigned char>(c) >= 32) {
                out += c;
            } else {
                out += ' ';
            }
        }
    }
    return out;
}

// Send Feedback's banner (menus.xml, Extra Buttons); origin is relative to that row. Send Feedback is at -300 90,
// 64 high, so -300 170 is right above it.
// The texture has the feedback icon baked in (x 290-318, y 11-53 of 336x64). A banner-colored patch covers it with a
// "</>" on top: two children swapped on hover (a state holds one image and one text). The click resets them: a menu
// hidden under the cursor never gets its hover end.
// restart: a "!" badge and a tooltip. The XML is rebuilt at every GUI load, which follows each Mods-menu change.
std::string MainMenuButton(bool restart)
{
    auto icon = [](const char* name, bool hidden, const char* patch, const char* glyph) {
        const char* hiddenAttr = hidden ? " hidden=\"true\"" : "";
        // clang-format off
        return std::string("<StaticImage name=\"") + name + "\" origin=\"136 0\"" + hiddenAttr + ">" +
                   Square(34, 48, patch) +
                   "<StaticText text=\"&lt;/&gt;\" font=\"header_4\" textColor=\"" + glyph + "\""
                   " sizeX=\"34\" fontAutoDownsize=\"true\"/>" +
               "</StaticImage>";
        // clang-format on
    };
    auto label = [](const char* color) {
        // clang-format off
        return std::string("<ButtonText align=\"c\" origin=\"-10 0\" sizeX=\"240\" text=\"MODLOADER\""
                           " font=\"header_4\" fontAutoDownsize=\"true\" textColor=\"") + color + "\"/>";
        // clang-format on
    };
    auto action = [](const char* type, const char* target) {
        return std::string("<Action type=\"") + type + "\" target=\"" + target + "\"/>";
    };
    const char* tooltip = restart ? "Restart the game to apply your plugin changes" : "Plugins: what loaded, settings";

    std::ostringstream s;
    // clang-format off
    s << "<Button name=\"" << kMainMenuButton << "\" hidden=\"true\" align=\"l\" origin=\"-300 170\""
      << Tooltip(tooltip) << ">"
          << "<RenderObject2D texture=\"" << kTex << "menu_feedback_normal.dds\"/>" << label("40382b")
          << icon(kButtonIcon, false, "f1e2ca", "40382b")
          << icon(kButtonIconHover, true, "41392b", "f0e3cc");
    // clang-format on
    if (restart) {
        // clang-format off
        s << "<StaticImage name=\"" << kRestartBadge << "\" origin=\"-146 0\">"
              << Square(30, 30, "f0a377")
              << "<StaticText text=\"!\" font=\"header_4\" textColor=\"201f1b\"/>"
          << "</StaticImage>";
        // clang-format on
    }
    // clang-format off
    s << "<OnHover>"
          << "<RenderObject2D texture=\"" << kTex << "menu_feedback_hover.dds\"/>" << label("f0e3cc")
          << action("Show", kButtonIconHover) << action("Hide", kButtonIcon)
      << "</OnHover>"
      << "<OnHoverEnd>" << action("Hide", kButtonIconHover) << action("Show", kButtonIcon) << "</OnHoverEnd>"
      << "<OnClick>"
          << "<RenderObject2D texture=\"" << kTex << "menu_feedback_hover.dds\" scaleX=\"0.95\" scaleY=\"0.95\"/>"
          << label("f0e3cc")
          << action("Hide", kButtonIconHover) << action("Show", kButtonIcon)
          << action("Hide", "Menu_Main") << action("Show", kScreen)
      << "</OnClick>"
      << "</Button>";
    // clang-format on
    return s.str();
}

const char* PageButtonLabel(PageButton b)
{
    switch (b) {
    case PageButton::WorkshopPage: return "Workshop page";
    case PageButton::ModFolder: return "Mod folder";
    case PageButton::SettingsFolder: return "Settings folder";
    case PageButton::OpenLog: return "Open dk2ml.log";
    }
    return "";
}

namespace {

// The screen frame, like Menu_Options / Menu_Mods. Leaves the screen item open.
void AddFrame(std::ostringstream& x)
{
    // clang-format off
    x << "<Item name=\"" << kScreen << "\" origin=\"0 0\" hidden=\"true\">"
      << "<OnKeyDown key0=\"27\"><Action type=\"Click\" target=\"" << kBack << "\"/></OnKeyDown>"
      << "<OnOpen>"
          << "<Action type=\"TriggerEvent\" target=\"GUI_CAPTURE_INPUT\"/>"
          << "<Action type=\"TriggerEvent\" target=\"GUI_GAME_last\" iParam=\"" << kEventOpen << "\"/>"
      << "</OnOpen>"
      << "<OnClose><Action type=\"TriggerEvent\" target=\"GUI_RELEASE_INPUT\"/></OnClose>"
      << "<StaticImage origin=\"0 0\">"
          << "<RenderObject2D texture=\"" << kTex << "bg_texture.dds\""
             " sizeX=\"7680\" color=\"f0e3cc26\" sizeY=\"1920\" texRepeatX=\"3\" texRepeatY=\"1\"/>"
      << "</StaticImage>"
      << "<StaticImage origin=\"0 0\">"
          << Square(2560, 9999, "0c0b0bbf")
          << "<StaticImage align=\"l\" origin=\"-128 0\">"
              << "<RenderObject2D texture=\"" << kTex << "bg_edge.tga\" flipX=\"true\"/>"
          << "</StaticImage>"
          << "<StaticImage align=\"r\" origin=\"128 0\">"
              << "<RenderObject2D texture=\"" << kTex << "bg_edge.tga\" flipY=\"true\"/>"
          << "</StaticImage>"
      << "</StaticImage>";
    // clang-format on
}

// The footer: Back (as in Options) and a note line.
void AddFooter(std::ostringstream& x)
{
    // clang-format off
    x << "<Item align=\"sv\" origin=\"0 -688\">"
          << "<StaticImage align=\"b\" origin=\"0 -32\">" << Square(9999, 120, "211e1d") << "</StaticImage>"
          << "<Button name=\"" << kBack << "\" align=\"lb\" origin=\"-1200 -4\">"
              << "<ButtonText align=\"l\" origin=\"20 0\" text=\"@menu_generic_back\""
                 " font=\"header_2\" textColor=\"f0e3cc\"/>"
              << "<OnHover>"
                  << "<RenderObject2D texture=\"" << kTex << "button_hover_01.tga\" color=\"f0e3cc\" flipX=\"false\"/>"
                  << "<ButtonText align=\"l\" origin=\"20 0\" text=\"@menu_generic_back\""
                     " font=\"header_2\" textColor=\"211e1d\"/>"
              << "</OnHover>"
              << "<OnClick>"
                  << "<RenderObject2D texture=\"" << kTex << "button_hover_01.tga\" color=\"40392b\" flipX=\"false\"/>"
                  << "<Action type=\"Hide\" target=\"" << kScreen << "\"/>"
                  << "<Action type=\"Show\" target=\"Menu_Main\"/>"
              << "</OnClick>"
          << "</Button>"
          << Text(kNote, "b", 0, 20, "", "paragraph_2", "f0a377")
      << "</Item>";
    // clang-format on
}

// The content area with the title bar. Leaves the content item open.
void AddContentArea(std::ostringstream& x)
{
    // clang-format off
    x << "<Item sizeX=\"2000\" sizeY=\"1200\" origin=\"0 90\">"
      << "<StaticImage align=\"t\" origin=\"0 0\" clipChildren=\"true\">"
          << Square(2020, 100, "f97b03")
          << "<StaticImage origin=\"0 0\" align=\"rt\">"
              << "<RenderObject2D texture=\"" << kTex << "deploy/deploy_class_diagonalbars.dds\""
                 " flipX=\"true\" sizeX=\"140\" sizeY=\"100\" color=\"0c0b0b33\"/>"
          << "</StaticImage>"
          << Text("", "c", 0, 0, "MODLOADER", "header_2", "201f1b")
      << "</StaticImage>";
    // clang-format on
}

// Left: the list of pages.
void AddModList(std::ostringstream& x, const std::vector<Page>& pages, std::vector<Widget>& widgets)
{
    // An ItemList places children from its center and ignores their align (Credits: 1160 high, first row at 0 540),
    // so row y = listHeight/2 - rowHeight/2 - row*pitch. Rows: 60 high, 70 pitch, 10 top margin, hence -40.
    const int modListHeight = kPanelHeight - 100;
    // clang-format off
    x << "<Item align=\"t\" sizeX=\"" << kListWidth << "\" sizeY=\"" << kPanelHeight << "\" origin=\"-720 -120\">"
          << "<StaticImage align=\"t\" origin=\"0 0\">"
              << Square(kListWidth, kPanelHeight, "211e1dB3")
          << "</StaticImage>"
          << PanelHeader(kListWidth, "MOD SETTINGS")
          << "<ItemList name=\"" << kModList << "\" align=\"t\" origin=\"0 -90\""
          << " sizeX=\"" << kListWidth << "\" sizeY=\"" << modListHeight << "\" direction=\"y\" clipChildren=\"true\">"
              << ScrollBar();
    // clang-format on

    for (size_t p = 0; p < pages.size(); ++p) {
        int row = static_cast<int>(p);
        int n = AddWidget(widgets, Widget::Kind::Select, row, 0);
        x << SelectRow(n, modListHeight / 2 - 40 - 70 * row, pages[p]);
    }

    x << "</ItemList></Item>";
}

// The page's fixed part (info, status, warnings, buttons, a rule). Returns the y below it.
int AddPageInfo(std::ostringstream& x, const Page& page, int pi, std::vector<Widget>& widgets)
{
    // Compact: the settings below get the room. Text origins are top edges.
    int y = -88;
    const int firstLine = y;
    for (const auto& line : page.lines) {
        // the status takes the right of the first line
        int width = page.hasStatus && y == firstLine ? 880 : 1320;
        x << Text("", "tl", 24, y, line, "paragraph_2", "f0e3cc",
                  " sizeX=\"" + std::to_string(width) + "\" fontAutoDownsize=\"true\"");
        y -= 34;
    }
    if (page.hasStatus) {
        // right-aligned, so no sizeX
        x << Text(StatusName(pi, true), "tr", -24, firstLine, "", "paragraph_2", "f97b03")
          << Text(StatusName(pi, false), "tr", -24, firstLine, "", "paragraph_2", "f0a377");
        if (page.lines.empty()) {
            y -= 34;
        }
    }
    for (const auto& line : page.warnLines) {
        x << Text("", "tl", 24, y, line, "paragraph_2", "f0a377", " sizeX=\"1320\" fontAutoDownsize=\"true\"");
        y -= 34;
    }

    if (!page.buttons.empty()) {
        y -= 8;
        for (size_t b = 0; b < page.buttons.size(); ++b) {
            int column = static_cast<int>(b);
            int n = AddWidget(widgets, Widget::Kind::Button, pi, column);
            x << FlatButton(WidgetName(n), "tl", 24 + 252 * column, y, 240, 44, PageButtonLabel(page.buttons[b]),
                            "paragraph_2", Event(n, kClick));
        }
        y -= 44 + 16;
    }

    if (page.hasStatus) {
        // a thin rule; plugins title their own sections with header rows
        x << "<StaticImage align=\"t\" origin=\"0 " << y << "\">" << Square(kPageWidth - 40, 2, "f0e3cc26")
          << "</StaticImage>";
        y -= 14;
        if (page.options.empty()) { // a mod not running declared none, but may have some
            x << Text("", "tl", 24, y, page.dimmed ? kSettingsWhenRunning : kNoSettings, "paragraph_2", "a08f80");
        }
    }
    return y;
}

// One settings row: a header strip, or a label (striped every other row) and the option's widget.
void AddOptionRow(std::ostringstream& x, const Option& opt, int row, int listHeight, int pi,
                  std::vector<Widget>& widgets)
{
    // from the list's center, like the mod list
    const int rowY = listHeight / 2 - kRowHeight / 2 - kRowHeight * row;
    x << "<Item origin=\"0 " << rowY << "\" sizeX=\"" << (kPageWidth - 40) << "\" sizeY=\"" << kRowHeight << "\">";

    if (opt.type == DK2ML_OPTION_HEADER) {
        x << "<StaticImage origin=\"0 0\">" << Square(kPageWidth - 40, kRowHeight - 4, "f97b03")
          << Text("", "l", 24, 0, opt.label, "header_4", "201f1b") << "</StaticImage>";
    } else {
        if (row % 2 == 1) {
            x << "<StaticImage origin=\"0 0\">" << Square(kPageWidth - 40, kRowHeight, "00000050") << "</StaticImage>";
        }
        if (opt.type != DK2ML_OPTION_BUTTON) {
            // no sizeX: a sized text box centers its text
            x << Text("", "l", 24, 0, opt.label, "paragraph_2", "f0e3cc", Tooltip(opt.tooltip));
        }
        int n = AddWidget(widgets, Widget::Kind::Option, pi, row);
        x << OptionWidget(opt, n, OptionsListName(pi));
    }

    x << "</Item>";
}

// A scrolling settings list from y to the page bottom.
void AddOptionsList(std::ostringstream& x, const Page& page, int pi, int y, std::vector<Widget>& widgets)
{
    const int listHeight = kPanelHeight + y - 20;
    // clang-format off
    x << "<ItemList name=\"" << OptionsListName(pi) << "\" align=\"t\" origin=\"0 " << y << "\""
      << " sizeX=\"" << kPageWidth << "\" sizeY=\"" << listHeight << "\" direction=\"y\" clipChildren=\"true\">"
          << ScrollBar();
    // clang-format on

    for (size_t o = 0; o < page.options.size(); ++o) {
        AddOptionRow(x, page.options[o], static_cast<int>(o), listHeight, pi, widgets);
    }

    x << "</ItemList>";
}

void AddPage(std::ostringstream& x, const Page& page, int pi, std::vector<Widget>& widgets)
{
    // clang-format off
    x << "<Item name=\"" << PageName(pi) << "\" align=\"t\" origin=\"0 0\""
      << " sizeX=\"" << kPageWidth << "\" sizeY=\"" << kPanelHeight << "\" hidden=\"true\">"
      << PanelHeader(kPageWidth, page.title);
    // clang-format on

    int y = AddPageInfo(x, page, pi, widgets);
    if (!page.options.empty()) {
        AddOptionsList(x, page, pi, y, widgets);
    }

    x << "</Item>";
}

// Right: one page per entry.
void AddPages(std::ostringstream& x, const std::vector<Page>& pages, std::vector<Widget>& widgets)
{
    // clang-format off
    x << "<Item align=\"t\" sizeX=\"" << kPageWidth << "\" sizeY=\"" << kPanelHeight << "\" origin=\"310 -120\">"
          << "<StaticImage align=\"t\" origin=\"0 0\">"
              << Square(kPageWidth, kPanelHeight, "211e1dB3")
          << "</StaticImage>";
    // clang-format on

    for (size_t p = 0; p < pages.size(); ++p) {
        AddPage(x, pages[p], static_cast<int>(p), widgets);
    }

    x << "</Item>";
}

} // namespace

Result Build(const std::vector<Page>& pages)
{
    Result r;
    std::ostringstream x;
    AddFrame(x);
    AddFooter(x);
    AddContentArea(x);
    AddModList(x, pages, r.widgets);
    AddPages(x, pages, r.widgets);
    x << "</Item></Item>"; // the content area, then the screen

    r.xml = x.str();
    return r;
}

} // namespace screenxml
