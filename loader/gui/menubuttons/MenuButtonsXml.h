// AddMenuButton's rules and its buttons as game GUI XML (see MenuButtons.cpp). No game names or hooks: screentest
// checks the XML, symtest's dry run uses Check.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "dk2ml.h"

namespace menuxml {

// The in-mission Esc menus (menus.xml, campaign.xml), each with an "Extra Buttons" row: Options at 0 0, How To Play at
// -80 0, Bug report at -160 0.
constexpr const char* kMenus[] = {"Menu_Ingame", "Menu_Ingame_Campaign"};
constexpr int kMenuCount = 2;
constexpr char kRow[] = "Extra Buttons"; // in other menus too, so looked up inside the menu

constexpr size_t kMaxText = 512; // bytes copied per text

// Slots: rows of kPerRow above Bug report / How To Play / Options, each filled from the left, then the next row up.
// kRows fit under the top of the 2560x1440 canvas (the row is at y -600) with the game's 80px edge margin.
constexpr int kPerRow = 3;
constexpr int kRows = 15;
constexpr int kSlots = kPerRow * kRows; // more are hidden
constexpr float kSlotSize = 80.0f;

// nullptr if fine, else why refused. structSize >= sizeof(DK2ML_MenuButton); extra bytes ignored. copy: the loader's.
const char* Check(const DK2ML_MenuButton* button, DK2ML_MenuButton* copy);

// The loader's copy of a button's texts. id: the button's number, unique for the session.
struct Button {
    int id;
    std::string tooltip, icon, iconHover, label;
};

std::string ContainerName(int id, int menu); // #dk2ml_mb<id>_<menu>: background and button, moved into the row
std::string ButtonName(int id, int menu); // its button; OnClick holds the Callback action

// Each button once per menu, hidden, at the top level (without the <GUIItems> wrapper)
std::string Items(const std::vector<Button>& buttons);

void SlotOrigin(int slot, float* x, float* y); // relative to the row

} // namespace menuxml
