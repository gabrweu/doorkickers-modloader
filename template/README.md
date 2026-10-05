# Plugin template

A starting point for a Door Kickers 2 plugin (a mod with code, run by dk2ml): a CMake project, a plugin that shows the
everyday pieces (`src/Plugin.cpp`), the mod folder (`mod/`), and scripts to build, test and package it.

## What's in here
| | |
|---|---|
| `src/Plugin.cpp`, `mod/` | your plugin and its mod folder (`mod.xml`, GUI files, ...) |
| `tools\build.ps1`, `install.ps1`, `package.ps1` | build, dev install (with a dry run), Workshop layout |
| `dk2ml\dk2ml.h`, `dk2ml.hpp` | the loader's C API and the optional C++ layer |
| `tools\symtest.exe` | the game's symbols explorer and your plugin's dry run (below) |
| `tools\disasm.ps1` | disassembly of game functions with names (optional, needs [LLVM](https://llvm.org)) |
| `docs\ExamplePlugin.cpp` | a minimal plugin in plain C |
| `docs\for-agents\` | the docs: rules, workflow, symbols, API map, code recipes (`cookbook.md`), troubleshooting. Written for coding agents, readable by people |
| `AGENTS.md`, `CLAUDE.md` | entry points that coding agents (Claude Code, Codex, Cursor, ...) pick up by themselves |
| `LICENSE` | the license (MIT) |

## Start
1. Unzip `dk2ml-template-for-devs-v<version>.zip` (from the loader's release page) somewhere of your own.
2. Rename the project in `CMakeLists.txt` (`project(my_plugin ...)`). The name is used for the DLL, the mod folder
   and your settings folder, so make it distinctive. Fill in `mod/mod.xml`, and the manifest at the top of
   `src/Plugin.cpp`.
3. Install Visual Studio 2022 (or its Build Tools) with the C++ workload.
4. `.\tools\build.ps1` builds `build\<name>.dll`.
5. `.\tools\install.ps1` (with the game closed) dry-runs the plugin with `tools\symtest.exe` and copies the mod to your
   `mods_upload` folder. Enable it once in the game's Mods menu, start the game, and read `dk2ml.log` in the game
   folder.

The game must have the Modloader (`dk2ml.dll` + `dbghelp.dll`) installed.

## Finding your way around the game
Everything is looked up by name from `DoorKickers2.pdb`, which ships with the game. `tools\symtest.exe` answers
questions about it:
```powershell
tools\symtest.exe "<game folder>" --find "GameClient::*Camera*"   # functions and globals (with the name to use for overloads)
tools\symtest.exe "<game folder>" --types "*Camera*"              # type names
tools\symtest.exe "<game folder>" --type Camera                   # a type's fields, offsets and sizes
tools\symtest.exe "<game folder>" --enum GameClient::eCGameState  # an enum's values
tools\symtest.exe "<game folder>" build\my_plugin.dll             # dry run: your DK2ML_PluginInit with nothing hooked
```
`docs\for-agents\` has the details: the rules (`rules.md`), how names map to code and how class results are returned
(`symbols.md`), every API call (`api-map.md`), code recipes (`cookbook.md`) and fixes (`troubleshooting.md`).

## Publishing
`.\tools\package.ps1` lays out `dist\<name>\` (mod files + `native\<name>.dll`). Copy it to
`%LOCALAPPDATA%\KillHouseGames\DoorKickers2\mods_upload\<name>` and upload it from the game's Mods menu. Most mods
also ship a `mod_image.jpg` preview in the mod folder. On the Workshop page, tell players they need the Modloader and
link it. Players are asked for permission before your code first runs, and again after each update.
