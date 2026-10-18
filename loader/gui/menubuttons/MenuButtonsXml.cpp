#include "MenuButtonsXml.h"

#include <algorithm>
#include <cstring>
#include <sstream>

#include "gui/nativemods/NativeModsScreenXml.h" // screenxml::Escape

// The look of the game's Esc-menu buttons (menus.xml): a 76x76 dark square behind a 72x72 button whose 64x64 icon
// shrinks while pressed. A label button draws its own body in the Modloader screen's palette (NativeModsScreenXml.cpp).

namespace menuxml {

namespace {

// The minimum structSize; bytes past sizeof(DK2ML_MenuButton) are ignored.
constexpr uint32_t kMinSize = offsetof(DK2ML_MenuButton, user) + sizeof(void*);

constexpr int kBackSize = 76;
constexpr int kButtonSize = 72;
constexpr int kIconSize = 64; // the game's menu_*.dds are 64x64, drawn unsized
constexpr const char* kTex = "data/textures/gui/";

bool HasText(const char* s)
{
    return s && s[0];
}

std::string Square(int size, const char* color, const char* extra = "")
{
    return std::string("<RenderObject2D texture=\"") + kTex + "square.tga\" sizeX=\"" + std::to_string(size) +
           "\" sizeY=\"" + std::to_string(size) + "\" color=\"" + color + "\"" + extra + "/>";
}

std::string Icon(const std::string& texture, const char* extra = "")
{
    return "<RenderObject2D texture=\"" + screenxml::Escape(texture) + "\" sizeX=\"" + std::to_string(kIconSize) +
           "\" sizeY=\"" + std::to_string(kIconSize) + "\" blendMode=\"none\"" + extra + "/>";
}

std::string Label(const std::string& text, const char* color)
{
    return "<ButtonText align=\"c\" text=\"" + screenxml::Escape(text) + "\" font=\"header_4\" sizeX=\"" +
           std::to_string(kIconSize - 8) + "\" fontAutoDownsize=\"true\" textColor=\"" + color + "\"/>";
}

std::string OneButton(const Button& b, int menu)
{
    const char* pressed = " scaleX=\"0.95\" scaleY=\"0.95\"";
    std::string container = ContainerName(b.id, menu);
    std::string normal, hover, click;
    if (!b.icon.empty()) {
        const std::string& hoverIcon = b.iconHover.empty() ? b.icon : b.iconHover;
        normal = Icon(b.icon);
        hover = Icon(hoverIcon);
        click = Icon(hoverIcon, pressed);
    } else {
        normal = Square(kIconSize, "40392b") + Label(b.label, "f0e3cc");
        hover = Square(kIconSize, "f97b03") + Label(b.label, "201f1b");
        click = Square(kIconSize, "f97b03", pressed) + Label(b.label, "201f1b");
    }
    std::string tooltip = b.tooltip.empty() ? "" : " tooltip=\"" + screenxml::Escape(b.tooltip) + "\"";

    std::ostringstream s;
    // clang-format off
    s << "<Item name=\"" << container << "\" origin=\"0 0\" hidden=\"true\">"
      << "<StaticImage origin=\"0 0\">" << Square(kBackSize, "0c0b0b80") << "</StaticImage>"
      << "<Button name=\"" << ButtonName(b.id, menu) << "\" origin=\"0 0\" sizeX=\"" << kButtonSize << "\" sizeY=\""
          << kButtonSize << "\"" << tooltip << ">"
          << normal
          << "<OnHover>" << hover << "</OnHover>"
          << "<OnClick>" << click << "<Action type=\"Callback\" target=\"" << container << "\"/></OnClick>"
      << "</Button>"
      << "</Item>";
    // clang-format on
    return s.str();
}

} // namespace

const char* Check(const DK2ML_MenuButton* button, DK2ML_MenuButton* copy)
{
    *copy = {};
    if (!button || button->structSize < kMinSize) {
        return "unknown button layout (set structSize = sizeof(DK2ML_MenuButton))";
    }

    memcpy(copy, button, std::min<size_t>(button->structSize, sizeof(*copy)));
    if (!copy->onClick) {
        return "no onClick";
    }
    if (!HasText(copy->icon) && !HasText(copy->label)) {
        return "neither an icon nor a label";
    }
    return nullptr;
}

std::string ContainerName(int id, int menu)
{
    return "#dk2ml_mb" + std::to_string(id) + "_" + std::to_string(menu);
}

std::string ButtonName(int id, int menu)
{
    return ContainerName(id, menu) + "_btn";
}

std::string Items(const std::vector<Button>& buttons)
{
    std::string xml;
    for (const Button& b : buttons) {
        for (int m = 0; m < kMenuCount; ++m) {
            xml += OneButton(b, m);
        }
    }
    return xml;
}

void SlotOrigin(int slot, float* x, float* y)
{
    *x = kSlotSize * static_cast<float>(slot % kPerRow - (kPerRow - 1));
    *y = kSlotSize * static_cast<float>(1 + slot / kPerRow);
}

} // namespace menuxml
