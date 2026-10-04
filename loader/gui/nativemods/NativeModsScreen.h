// The "Native mods" screen at runtime (NativeModsScreen.cpp). The XML itself is built by NativeModsScreenXml.cpp.
#pragma once

#include <string>

#include "Loader.h"

// The screen's GUI items (without <GUIItems>), from the loader's records; also remembers the pages for the runtime.
std::string Screen_BuildItems();
void Screen_OnGuiLoaded(); // after GUIManager::Load
void Screen_Tick();        // every frame: key capture for KEY options
void Screen_OnGameEvent(void* params); // the game's GUI event 219 (GUI_GAME_last), from GuiKit.cpp's consumer
