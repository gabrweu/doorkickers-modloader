# dk2ml: Door Kickers 2 native mod loader
dk2ml lets mods for *Door Kickers 2: Task Force North* ship code (DLLs), which makes much more complex mods possible.

### WHY?
Door Kickers 2's built-in modding is limited. I'm a big fan of the game, and some of its deeper annoyances looked
easy to fix with code-level modding.

### HOW?
The loader takes the place of `dbghelp.dll`, a DLL the game loads at startup, and forwards the game's own calls to
the real one in System32.

It then connects mod DLLs to the game's code. Game functions, globals, struct fields and enums are found **by name**
in the `DoorKickers2.pdb` the game ships with, so mods don't depend on addresses that move between game builds. The
loader changes nothing about how the game plays and modifies no game files. It hooks a few game functions in memory
and passes them to mods as events. Mods can also hook and call game functions themselves.

Native mods are distributed through the Steam Workshop like any other mod. Their code runs only after the player
enables the mod in the game and allows it in a permission prompt. The prompt comes back whenever the mod's code
changes.

------

> [!WARNING]
> DLLs loaded through the Steam Workshop (or downloaded by hand) are not limited to the loader's API. **They can run
> any code and can make your machine vulnerable.** You give someone else's code the same access to your system that
> the game has. That is the cost of this level of moddability.
>
> The loader asks before running a Workshop mod's code, and again after every update. It does nothing more. Reading
> the mod's source code **and** decompiling the shipped DLLs are good steps to take, but there's no realistic way to
> guarantee the quality, reliability or safety of arbitrary mod DLLs.
>
> ⚠️ **DO NOT RUN MODS FROM SOURCES YOU DO NOT TRUST.** ⚠️

------

Plugins are separate mods. For example, Free Camera (its own repository and Workshop item) lets you rotate, tilt and
zoom the mission camera.

## For players
1. **Install the loader once.** Download `DK2-NativeModLoader` from [Releases](../../releases) and close the game. In
   Steam, right-click Door Kickers 2 > Manage > Browse local files, and copy `dk2ml.dll` and `dbghelp.dll` into that
   folder.
2. **Get plugins from the Steam Workshop** (e.g. *Free Camera*). Subscribe, then enable or disable each one in the
   game's Mods menu like any other mod. The game applies that menu without restarting, but code loads only at
   startup. A native mod you disable is switched off immediately, and one you enable runs from the next start. The
   **Native mods** button shows a "!" while a change waits for a restart.
3. **Answer the prompt.** When Workshop mods with code are new or updated since your last answer, the loader asks
   once at startup and lists them by name. Say Yes only to mods you trust. Answers are kept in `dk2ml.ini` in the
   game folder. Delete a mod's line to be asked again.

**Native mods screen.** The main menu gets a **Native mods** button (bottom right, above "Send Feedback"). Its screen
uses the game's own GUI elements and lists every enabled mod that contains native code. For each one it shows where
it comes from, whether its code loaded (and why not), its settings, and buttons to open its Workshop page or folders.

**Something wrong?** If a native mod keeps the game from starting, set `enabled=0` under `[loader]` in `dk2ml.ini`
(game folder) to start without native mods. After a crash, `dk2ml-crash-*.log` in the game folder names the mods
whose code was involved. To uninstall, delete `dk2ml.dll` and `dbghelp.dll` from the game folder. The release zip's
README.txt ([`docs/players.txt`](docs/players.txt)) covers troubleshooting.

## Writing a plugin

### Why use the loader instead of your own
You could ship your own proxy DLL and MinHook. Here is what you would have to build (and get right) yourself:

- **Players install it once.** Only one file can be the game's `dbghelp.dll`, so two mods that each bring their own
  injector overwrite each other. With dk2ml, your mod is an ordinary Workshop item. Players enable it in the Mods
  menu, and it's switched off as soon as they disable it.
- **Hooks that don't corrupt the game.** The game is built with link-time code generation, so callers keep values in
  registers that a plain detour is allowed to overwrite. A pass-through MinHook detour on `Camera::SetDefaults` baked
  a black shadow line into every map. Safe hooks preserve every register, and a test (`hooktest`) checks that a
  pass-through hook changes nothing. See "Use safe hooks" below.
- **Game updates break less, and loudly.** Functions, globals, fields and enums are found by name in the PDB the game
  ships, not by address or pattern. The loader also handles the PDB's traps:
  - Many types exist twice in the PDB, once with a stale 32-bit layout. The loader picks the copy that fits this build.
  - The linker folds identical functions (one body can serve 20 names). The loader warns when you hook one of those.
  - Some names are overloaded. The loader warns when you resolve one of those without picking the overload.

  If an update removes a name you use, your init fails cleanly. Your mod's page on the Native mods screen then tells
  the player which names are missing, instead of the game crashing.
- **Room for other mods.** Any number of plugins can hook the same function. Frames, state changes, map loads, GUI
  reloads and window resizes arrive as events, so most plugins need no hooks of their own. The loader logs the
  conflicts it can see (shared hooks, skipped originals, clashing dependency DLLs, shared hotkeys). Plugins can offer
  each other functions through interfaces. See "Many mods at once" below.
- **Your bugs don't end the session.** A crash in your init, hooks, events, tasks, options or GUI callbacks is caught,
  logged and switches your plugin off, and the game carries on. When the game does crash, `dk2ml-crash-*.log` names
  the mods whose code was on the stack. That is hard to do on your own: at startup the game patches
  `SetUnhandledExceptionFilter` into a no-op, so nobody else can install a crash handler.
- **Settings and UI without GUI work.** `AddOption` puts checkboxes, sliders, choices, key bindings and buttons on your
  mod's page of the Native mods screen, drawn with the game's own widgets. On top of that, the GUI kit drives the
  game's items, `CaptureGameInput` keeps clicks off the map, and `GetConfigDir` gives you a settings folder that
  survives Workshop updates.
- **One consent prompt players already know.** Workshop code runs only after the player allows it, and again after
  every update. It's one prompt from a loader whose source is public, instead of a different scheme per mod. See
  "Consent and robustness" below.
- **Tools.** `symtest.exe` explores the PDB and dry-runs your init against it before you start the game. There's also
  a cookbook of recipes and pitfalls, and a template that builds, installs and packages.

**What it costs you.** Players need the loader installed, and you write to its C API (`DK2ML_API_VERSION`; your
manifest says which version you need). Nothing else is fenced off: your plugin is an ordinary DLL. It can use any
library, call any game function and start its own threads. Code on your own threads isn't crash-contained.

### Tools and API
Everything a mod maker needs is in `DK2-NativeModTemplate` on the [Releases](../../releases) page, a separate zip
from the loader:
1. **The template:** a CMake project with a plugin that shows the everyday pieces, a mod folder, and build,
   dev-install and package scripts. It builds where you unzip it, with the headers in `dk2ml\`, `symtest.exe` in
   `tools\` and the cookbook in `docs\`. Its source is [`template/`](template), which only builds from the zip.
2. **The explorer:** `symtest.exe "<game folder>" --find "GameClient::*"` (also `--types`, `--type`, `--enum`) shows
   the game's functions, globals, type layouts and enums from its PDB.
3. **The dry run:** `symtest.exe "<game folder>" <your.dll>` runs your init against the real PDB before you start the
   game. It must print `plugin init returned 0` and exit 0. Exit code 100 means init succeeded but the loader would
   refuse a call (`PROBLEM` lines).
4. **The cookbook:** [`docs/cookbook.md`](docs/cookbook.md) covers game state, calling and hooking game functions,
   events, settings, GUI, ImGui, the common pitfalls, debugging, game updates and publishing.

A plugin is a 64-bit DLL in `<your mod>\native\` that exports `DK2ML_PluginInit`. It includes
[`include/dk2ml.h`](include/dk2ml.h) (the C API) and optionally [`include/dk2ml.hpp`](include/dk2ml.hpp), a
header-only C++ layer for declaring game names once, resolving them all, and typed hook arguments. A minimal plugin
without the C++ layer is [`docs/ExamplePlugin.cpp`](docs/ExamplePlugin.cpp). How the loader works inside is in
[`docs/overview.md`](docs/overview.md).

```cpp
#include "dk2ml.h"

// GameClient::UpdateCamera(this, int dt): this = DK2ML_Arg(regs, 0) (rcx), dt = DK2ML_Arg(regs, 1) (rdx)
static int Pre(DK2ML_Regs* regs, void* user) { /* ... */ return DK2ML_CALL_ORIGINAL; }
static void Post(DK2ML_Regs* regs, void* user) { /* after the original; regs->rax = its result */ }

DK2ML_EXPORT int DK2ML_PluginInit(const DK2ML_API* api, const DK2ML_PluginInfo* info)
{
    void* target = api->ResolveSymbol("GameClient::UpdateCamera");
    if (!target || api->CreateSafeHook(target, Pre, Post, NULL) != DK2ML_OK)
        return 1;
    api->EnableHook(target);
    return 0;
}
```

**Use safe hooks (`CreateSafeHook`) for game functions, never plain detours.** The game is built with link-time code
generation, so callers keep values in registers that the x64 calling convention lets a call overwrite. A plain
detour overwrites them and silently corrupts the caller. A safe hook saves every register, runs your callbacks, then
restores the registers and runs the original exactly as the caller set it up. The cookbook's "Hooking" section has
the details.

**Many mods at once.** The loader hooks the game's common moments once and sends them to every plugin as events, so
plugins need no hooks of their own for frames, state changes, map loads or GUI reloads. Any number of plugins can
safe-hook the same function: the loader keeps one hook with a chain of callbacks. It reports the conflicts it can see:
- in `dk2ml.log`: functions several mods hook (in order), a mod that skips a function others hook, and dependency
  DLLs that two mods ship in different versions;
- on the Native mods screen: dependency DLL clashes and the same hotkey in two mods.

Two mods that want opposite things from the same game behavior still need their authors to agree. See the cookbook's
"Living with other mods".

| API | |
|---|---|
| `ResolveSymbol(name)` | function or global address. Undecorated name (`"Camera::UpdateViewMatrix"`), or the decorated name (`"?FindChild@Item@GUI@@QEAAPEAV12@PEBD@Z"`) to pick one overload |
| `GetFieldOffset(type, field)` / `GetTypeSize(type)` | struct layouts, e.g. `("Camera", "m_rotAngles")`. For a bitfield, the offset of its storage unit |
| `GetEnumValue(enum, name, &v)` | an enumerator by its C++ name, e.g. `("GUI::eAction", "ACTION_ADD_CHILD")` |
| `CreateSafeHook(target, pre, post, user)` | register-preserving hook. Several plugins can hook the same function |
| `EnableHook` / `DisableHook` | turn your hook on or off (hooks start disabled) |
| `Log(fmt, ...)` | a line in `dk2ml.log`, prefixed with your DLL's name |
| `GetGameDir()`, `IsGameFocused()` | game folder; whether the game window has focus |
| `GetConfigDir()` | `%LOCALAPPDATA%\KillHouseGames\DoorKickers2\dk2ml\<your dll name>\`, for settings. Never write to your mod folder: Steam replaces it on every Workshop update |
| `AddOption(&option)` | a setting on your mod's page of the Native mods screen: header, checkbox, slider, left/right choice, key binding or button |
| `Subscribe(type, fn, user)` / `GetGameState()` | events: `FRAME`, `STATE_CHANGED`, `MAP_LOADED`, `GUI_LOADED`, `PLUGINS_LOADED`, `WINDOW_RESIZED`; the current `GameClient::m_state` |
| `SubscribeGuiEvent(id, fn, user)` | one of the game's GUI events (`GUI::Events::eEventType`) as a `GUI_EVENT`, after the game handled it |
| `PublishInterface(name, version, table)` / `GetInterface(name, minVersion, &version)` | function tables plugins offer each other by name: published in init, looked up from `PLUGINS_LOADED` on |
| `AddTask(fn, user)` | run `fn` on the main thread at the start of the next frame; callable from any thread |
| `GuiFind`, `GuiParent`, `GuiName`, `GuiChildren`, `GuiIsShown`, `GuiShow`, `GuiSetText`, `GuiAddChild`, `GuiSetOrigin`, `GuiClick` | the GUI kit: the game's GUI items, driven by the game's own functions and actions |
| `GuiSetCallback(item, itemEvent, fn, user)` | points an item's XML `<Action type="Callback">` at your function |
| `CaptureGameInput(1/0)` / `IsGameMenuOpen()` | keep clicks and keys off the map while your UI is open; whether one of the game's own menus is open |
| `GetGameVersion()` / `GetGameWindow()` | the game's version as written in its saves (e.g. 112); its main window |
| `DK2ML_PLUGIN_MANIFEST(minApi, name, version, author, url, gameVersion)` | plugin name, version and author, read without running the plugin and shown on the Native mods screen, in the log and in crash reports. A loader whose API version is below `minApi` doesn't load the plugin and says why |

Every callback the loader makes into a plugin runs under crash containment. `DK2ML_PluginInit` runs before any game
code, so install hooks there.

**Publishing a plugin.** Ship it as a regular mod folder (`mod.xml`, plus your DLL in `native\`) and upload it from
the game's Mods menu. Players install the loader once and enable your mod like any other.

## Consent and robustness
Native mods are ordinary code with full access to the player's PC. The loader can't make that safe and doesn't try.
It asks before running Workshop code, contains the crashes it can, and reports what happened.

**Where code may come from**
- Mod folders are resolved to their real path first (junctions, symlinks and `..`).
- The player's own folders load without asking: `%LOCALAPPDATA%\KillHouseGames\DoorKickers2\mods\` and
  `mods_upload\`, plus `<game>\mods_native\`.
- Door Kickers 2 Workshop items in the game's own Steam library need consent.
- Anything else is skipped and logged.

**Consent per version**
- At startup, one dialog lists the Workshop items whose code is new or changed since the last answer, with their
  title (from the mod's `mod.xml`), item id and a warning. Yes loads them all; No skips them all.
- Each answer is stored with a SHA-256 fingerprint of the item's entire `native\` folder, every file and subfolder:
  `dk2ml.ini` `[workshop]` `<itemId>=allow:<sha256>` or `deny:<sha256>`.
- Any change to the folder asks again, and so does deleting the line. `allow_workshop_plugins=1` turns the prompt off
  (not recommended).
- Allowed code loads from the Workshop folder itself.

**Crash containment**
- If a plugin's init or any loader callback into it (hooks, events, tasks, options, GUI callbacks) crashes, the loader
  catches it, logs it and switches that plugin off. Its hooks pass through and the game carries on.
- A switched-off plugin can't hook again: `CreateSafeHook`, `EnableHook` and `GuiSetCallback` refuse it, even from
  its own threads.
- Not contained: a plugin's own threads, its `DllMain`, and damage done before the crash. A crash in a plugin's
  `DllMain` turns all native mods off for that session, and the game starts unmodded.

**Crash reports**
- When the game crashes with an unhandled exception, the loader writes `dk2ml-crash-<date>-<time>.log` in the game
  folder just before the game's own crash dump. It covers what crashed and where, which mods' code is on the stack,
  the stack with game functions named from the PDB (hooked ones marked with their hooks' owners), the registers, and
  the mod list.
- It comes from a safe hook on the game's crash handler (`CreateMiniDump`), which the game makes the only top-level
  exception filter.
- Not covered: a crash below a post-hooked function, which can't be traced up to the crash handler.

**Logging.** Everything goes to `<game folder>\dk2ml.log`. The previous session's log is kept as `dk2ml.prev.log`, so
it survives the restart after a crash.

## Building
Needs Visual Studio 2022 Build Tools (C++ workload, which includes CMake, Ninja and MASM).
```powershell
.\tools\build.ps1        # Release build -> build\dk2ml.dll + dbghelp.dll, symtest.exe, the tests and test plugins
ctest --test-dir build   # the seven tests; each must pass (from a VS developer prompt)
.\tools\install.ps1      # dev install: build\dk2ml.dll + dbghelp.dll -> game folder
.\tools\format.ps1       # clang-format the sources (-Check only reports); needs LLVM or VS's Clang tools
```
`tools/gen_exports.ps1` regenerates the `dbghelp.dll` stub's exports from System32's dbghelp.dll.

For disassembly, with [LLVM](https://llvm.org) installed:
```powershell
llvm-pdbutil dump -publics "<game>\DoorKickers2.pdb" > re\publics.txt  # every function/global, decorated
tools\disasm.ps1 -Names '?UpdateCamera@GameClient'                     # disassembly with call targets named
```
These dumps are derived from the game and are git-ignored. Don't publish them.

GitHub Actions builds and runs the tests on every push and pull request (`.github/workflows/ci.yml`).

## Releasing
1. Set the version in `project(dk2ml VERSION ...)` in `CMakeLists.txt`, the only place it's defined. Commit and push
   to the default branch.
2. Actions > **Release** > Run workflow, on the default branch. It builds, runs the tests and packages:
   - `DK2-NativeModLoader-v<version>.zip`: the two DLLs, README.txt from `docs/players.txt`, and the licenses;
   - `DK2-NativeModTemplate-v<version>.zip`: `template/` with the headers, `symtest.exe`, `disasm.ps1`, the cookbook,
     the C example and the license;
   - a `.sha256.txt` with the hashes.

   It then creates the tag `v<version>` and publishes the release. A version that already has a tag is refused.

## License
MIT (see [LICENSE](LICENSE)). Includes [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause).
Door Kickers 2 is © KillHouse Games; this project isn't affiliated with them.
