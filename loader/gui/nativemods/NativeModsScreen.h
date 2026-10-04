// The "Native mods" screen at runtime. NativeModsScreenXml.cpp builds the XML.
#pragma once

#include <string>

#include "Loader.h"

// The screen's items (no <GUIItems>) from the loader's records; also keeps the pages for the runtime.
std::string Screen_BuildItems();
void Screen_OnGuiLoaded(); // after GUIManager::Load
void Screen_Tick();        // every frame: key capture for KEY options
void Screen_OnGameEvent(void* params); // GUI event 219 (GUI_GAME_last), from GuiKit.cpp's consumer
