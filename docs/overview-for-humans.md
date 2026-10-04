# dk2ml overview

Instructions for players to install the modloader are in the [README](../README.md).

### What it is
Door Kickers 2's own modding gets you XML and assets, but no code, so we need to somehow load external code some other way. With this modloader we can do that, allowing mods to ship DLLs, and the loader then _loads_ them into the game.

The whole loader is two files in the game folder: `dk2ml.dll` (the loader) and `dbghelp.dll` (a tiny stub). Mods with code ("plugins") are normal game mods with a `native\` folder, distributed through the Workshop like everything else.

### How it works
First, the game imports `dbghelp.dll` at startup, so Windows loads our stub before any game code runs. The stub forwards every real dbghelp call to the one in `System32` and loads `dk2ml.dll` next to it. That's it, no game files are touched. The loader hooks the game's entry point, and when that runs it loads the symbols and the plugins. Only then does the game itself start. If the loader itself crashes during all this, it turns everything off, shows a message box, and the game starts unmodded.

The game ships with its `DoorKickers2.pdb`, which is a gift. It means every function, global, struct field and enum value can be looked up by name. Nothing in the loader or in plugins hardcodes an address or an offset, so most game updates don't break anything. When an update does remove a name, the plugin that needs it fails to start cleanly, and its page in the game tells the player which names are missing.

### Hooks
The game is compiled with link-time code generation, and the compiler knows exactly which registers each function really uses. So callers keep values in registers that the normal x64 rules say a function may overwrite. A regular C++ detour overwrites them, and the caller quietly breaks. In Free Camera, a hook that did nothing except pass the call through baked a black shadow line into every map.

So every hook goes through `CreateSafeHook`, which saves and restores every register around the mod's code. Plugins get a "pre" callback before the original function and a "post" callback after it. Any number of mods can hook the same function.

### Events
Mission starting, GUI reloading, frames, etc relevant. The loader hooks those once and hands them out as events (`FRAME`, `STATE_CHANGED`, `MAP_LOADED`, `GUI_LOADED`, `WINDOW_RESIZED`, `PLUGINS_LOADED`, and the game's own GUI events).

### What happens when you start the game
1. The stub loads the loader. The loader loads the game's PDB.
2. It reads which mods are enabled (the game's `options.xml`), plus anything in `<game>\mods_native\`.
3. Your own mod folders load right away. Workshop mods with code get one permission prompt listing all of them, and
   the answer is remembered per version of the mod's code. Anything else is skipped.
4. Each plugin's `DK2ML_PluginInit` runs, before the game has done anything. That's where plugins set up hooks,
   events and settings.
5. The game starts.

The game applies Mods-menu changes without restarting. The loader follows along as far as it can: disabling a mod switches its plugins off right away, but newly enabled plugins only load on the next start. The button gets a "!" when a restart is needed. I.e. for loading/unloading DLL mods we need a full restart of the game (not the auto restart from the prompt). Could do a custom `.exe` to avoid this but is what it is for now.

### Writing a plugin
Grab `dk2ml-template-for-devs-v<version>.zip` from the releases. It's a CMake project that builds where you unzip it, with the headers, the tools and the docs. The loop:

1. Rename the project in `CMakeLists.txt`. The name ends up on the DLL and the settings folder, so pick something
   unlikely to clash.
2. `.\tools\build.ps1` to build.
3. `.\tools\install.ps1` with the game closed. It dry-runs your plugin, then copies it into your `mods_upload` folder.
4. Enable the mod in the game's Mods menu, restart the game, and read `dk2ml.log` in the game folder.

Or just do your own scripts.

The smallest useful plugin looks something like this (in C++, you could of course use other languages as long as a `.dll` is the final product):
```cpp
#include "dk2ml.hpp"

DK2ML_PLUGIN_MANIFEST(1, "My Plugin", "1.0.0", "You", "", 112);

namespace {
const DK2ML_API* g_api = nullptr;
dk2ml::Enum Running{"GameClient::eCGameState", "CGAMESTATE_RUNNING"};

void OnStateChanged(const DK2ML_Event* e, void*)
{
    if (e->newState == Running.Get())
        g_api->Log("mission running");
}
} // namespace

DK2ML_EXPORT int DK2ML_PluginInit(const DK2ML_API* api, const DK2ML_PluginInfo*)
{
    g_api = api;
    if (!dk2ml::ResolveAll(api)) // logs every missing name
        return 1;
    return dk2ml::On(api, DK2ML_EVENT_STATE_CHANGED, OnStateChanged) ? 0 : 2;
}
```

### Tools
`symtest.exe` comes with the template. `symtest.exe "<game>" --find "GameClient::*"` searches the PDB, and `--type`, `--types` and `--enum` show layouts and values. `symtest.exe "<game>" your_plugin.dll` runs your init against the real PDB without the game. It catches wrong names, which is by far the most common bug. `disasm.ps1` disassembles game functions by name if you have LLVM installed.

### Safety
As you'd expect, running this means any mods from the Workshop that load a DLL can run whatever code they want in your machine. There isn't much we can do to avoid that, so be responsible.

### Living with other mods
People will run several of these mods at once. The loader keeps them out of each other's way where it can. Hooks on the same function form a chain, every plugin gets its own crash containment and settings folder, and plugins can talk through published function tables ("interfaces") instead of poking at each other's memory. It also warns about the things it can't fix: two mods shipping different DLLs with the same name, two plugins with the same file name, and two plugins defaulting to the same hotkey.

### The loader's code
Everything lives in `loader/`:

| Folder | What's in there |
|---|---|
| `core/` | startup (`DllMain.cpp`), the log, and the PDB lookups (`Symbols.cpp`) |
| `proxy/` | the `dbghelp.dll` stub: loads the loader and forwards all 252 real dbghelp exports |
| `hooks/` | safe hooks: the register-saving stubs and the callback chains |
| `plugins/` | finding and loading plugins, the API table, events, interfaces, the loader's own game hooks |
| `gui/` | the Native mods button and screen (built as game GUI XML), and the GUI helpers plugins can use |
| `safety/` | the Workshop permission prompt and fingerprints, and the crash reports |

Outside of it: `include/` has the public headers, `template/` the plugin project, `tools/` symtest and the build scripts, and `tests/` what `ctest` runs. MinHook is vendored in `tools/third_party/minhook/`.

Building it yourself needs Visual Studio 2022 Build Tools with the C++ workload. LLVM is optional, for disassembly.
Personally, I just use Visual Studio Code with the plugins to enable it to work with C++ and whatever tools are needed.

```powershell
.\tools\build.ps1                 # Release build into build\
ctest --test-dir build            # all tests
.\tools\install.ps1               # copies the two DLLs into the game folder (game closed)
.\tools\format.ps1                # clang-format over the sources
```

### Limits
Things the modloader doesn't do, or can't:
- A plugin's own threads aren't crash-contained, and nothing tracks what a plugin changes outside the API.
- A crash below a function some plugin post-hooks can't be traced back. It gets no crash report and no game dump.
- The permission prompt means trusting the author. Nothing checks what a mod's code actually does.
- A game update that removes a name a plugin needs stops that plugin until its author updates it.
