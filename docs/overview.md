# dk2ml overview

What the project is, how a game session runs with it, where the code for each part lives, and how plugins use it.
For player instructions and the full API table see [README.md](../README.md). For recipes see
[cookbook.md](cookbook.md).

## What it is
- A native (DLL) mod loader for *Door Kickers 2: Task Force North*.
- It ships as two files placed in the game folder: `dk2ml.dll`, the loader, and `dbghelp.dll`, a stub. The game
  statically imports `dbghelp.dll`, so Windows loads the stub before any game code runs, and the stub loads
  `dk2ml.dll` from its own folder. Every real dbghelp export is forwarded by a jump stub to `System32\dbghelp.dll`.
- It loads plugins: 64-bit DLLs shipped inside ordinary game mods.
- Game functions, globals, struct fields, type sizes and enum values are found by name in `DoorKickers2.pdb`, which
  the game ships. The loader and plugins hardcode no addresses or offsets, so most game updates don't break them.
- Plugins are separate projects and Workshop items, for example Free Camera.

## One session, start to finish

### 1. Process start: the stub (`loader/proxy/ProxyMain.cpp`), then `DllMain` (`loader/core/DllMain.cpp`)
1. The stub's `RealDbghelp_Load` loads `System32\dbghelp.dll` and fills the forwarding table.
2. If the process isn't `DoorKickers2.exe`, it stops here. Otherwise it loads `dk2ml.dll` by full path from its own
   folder; if that fails, it appends a line to `dk2ml.log` and the game starts unmodded.
3. In `dk2ml.dll`'s `DllMain`, MinHook hooks the exe's entry point with `EntryDetour`. That is all it does under the loader lock.

### 2. Before the game's own code: `EntryDetour` → `StartLoader`
`StartLoaderContained` runs `StartLoader` under `__try`. If the loader faults, every hook is disabled, a message box
explains, and the game starts unmodded. Steps:
1. `Consent_EnsureIni`: writes a default `dk2ml.ini` if there is none. `[loader] enabled=0` stops here.
2. `Symbols_Init`: loads the PDB (exact match required). If it fails, no plugins load.
3. `HookCrashHandler`: safe-hooks the game's `CreateMiniDump` so a crash writes `dk2ml-crash-*.log`, even when no
   plugin is installed.
4. `Plugins_LoadAll` (below).
5. `GameHooks_Init`: the loader's own game hooks (below), and through `Menu_Init` the Native mods button and screen.
6. `Plugins_LogSharedHooks`: logs every function hooked by more than one module.
7. Calls the original entry point. The game starts.

### 3. `Plugins_LoadAll` (`loader/plugins/Plugins.cpp`)
1. Collect mod folders: the enabled mods in `%LOCALAPPDATA%\KillHouseGames\DoorKickers2\options.xml`
   (`EnabledModDirs`) plus `<game>\mods_native\`.
2. `SplitDlls` on each `native\` folder. A DLL exporting `DK2ML_PluginInit` is a plugin; others are its
   dependencies and the loader doesn't load them. The optional manifest is read here. Both checks map the file as an
   image resource, so no plugin code runs.
3. `Consent_Classify` decides where the folder is:
   - **Local** (`<save>\mods\`, `<save>\mods_upload\`, `<game>\mods_native\`): loads.
   - **Workshop** (the game's own `steamapps\workshop\content\1239080\<id>\`): needs the player's answer.
   - **Unknown**: skipped.
4. `Consent_DecideAll`: fingerprints each Workshop item (SHA-256 of the whole `native\` folder) and checks
   `dk2ml.ini`. Items without an answer for their current code are listed in one Yes/No prompt, and the answer is
   saved for each. Allowed items load from their Workshop folder.
5. `CheckSupportDlls`: warns when two mods ship the same DLL name with different contents.
6. For each plugin:
   - skip if a plugin with the same file name already loaded (`Duplicate`);
   - skip if the manifest's `minApiVersion` is above this loader's API version (`NeedsNewerLoader`);
   - `LoadPlugin`: `LoadLibrary` (its `DllMain` runs there, uncontained), then `DK2ML_PluginInit` under `__try`.
7. Close registration (options, events, interfaces), log the interfaces, dispatch `DK2ML_EVENT_PLUGINS_LOADED`.

### 4. While the game runs: `GameHooks_Init` (`loader/plugins/GameHooks.cpp`)
The loader safe-hooks a few game functions once and turns them into events for every plugin:

| Game function | Used for |
|---|---|
| `ImGui::Render` | the frame tick: `Events_Frame` (queued tasks, `STATE_CHANGED`, `FRAME`), key capture |
| `GUIManager::Load`, `GUIManager::MergeItemsFromXML` | `Plugins_SyncEnabled` first (the Mods menu's changes, see below); merges the main-menu button and the "Native mods" screen into the GUI, re-registers GUI event consumers, `GUI_LOADED` |
| `Camera::SetDefaults` | `MAP_LOADED`, only when the call is for the GameClient's camera (only hooked if someone subscribed) |
| `GameGUI::IsAnyMenuOpened` | `CaptureGameInput`: reports a menu open while a plugin captures input |
| `GameRenderer::OnWindowResized` | `WINDOW_RESIZED` (only if subscribed) |

**The Mods menu during a session.** The game applies Mods menu changes without restarting. `Mods::SetModAsActive`
updates its active list (`g_modsInstance.m_activeMods`) on each click, then the game reloads its data and GUI. At the
start of each `GUIManager::Load`, `Plugins_SyncEnabled` compares that list with the loader's records, by folder key
(`Consent_DiffEnabled`):
- A running mod that left the list is switched off (`SwitchOff`, the same path as after a crash) and becomes
  `TurnedOff`. This applies only to a mod the game has listed before, because the list may be empty at the first
  GUI load.
- A newly listed mod with plugins is recorded as `EnabledLater`.
- Either change puts a "!" on the main-menu button (`Plugins_RestartNeeded`).
- Without the list's names, `options.xml` is re-read instead.

### 5. Exit or crash
- Unhandled crash: the `CreateMiniDump` pre writes the crash report, then the game's own dump runs.
- A crash in plugin code that the loader catches (init, safe-hook callback, event, task, option, GUI callback)
  switches that plugin off. The game keeps running. The API then refuses it new hooks and GUI callbacks, and its
  page on the Native mods screen says it crashed.

## The parts

### `loader/`
Folders: `core/` (startup, log, PDB), `proxy/` (the `dbghelp.dll` stub), `hooks/` (safe hooks), `plugins/` (loading
and the API), `gui/` (the game's GUI), `safety/` (Workshop consent, crash reports). `Loader.h` at the root
declares everything shared between files.

| File | Does | Start reading at |
|---|---|---|
| `core/DllMain.cpp` | process attach, entry-point hook, startup order, crash-handler hook | `DllMain`, `EntryDetour`, `StartLoader` |
| `core/Symbols.cpp` | PDB lookups: functions, globals, fields, type sizes, enums. Name index, overload and folded-code (ICF) warnings, picks the 64-bit copy of duplicated types | `Symbols_Init`, `Symbols_Resolve`, `Symbols_FieldOffset`, `Symbols_TypeSize`, `Symbols_EnumValue` |
| `core/Log.cpp` | `dk2ml.log`, previous session kept as `dk2ml.prev.log` | `LogF` |
| `proxy/ProxyMain.cpp`, `RealDbgHelp.cpp`, `Exports.asm`/`.def`, `ExportNames.inc` | the `dbghelp.dll` stub: loads `dk2ml.dll`; 252 jump stubs into the real DLL, generated by `tools/gen_exports.ps1`. `dk2ml.dll` uses `RealDbgHelp.cpp` too, for the PDB lookups | `DllMain` (stub), `RealDbghelp_Load`, `RealDbghelp` |
| `hooks/SafeHook.cpp`, `SafeHookEntry.asm` | register-preserving hooks: per-hook stub saves all registers, runs the chain of pre callbacks, restores, runs the original; posts through a swapped return address | `SafeHook_Create`, `SafeHook_SetEnabled`, `SafeHook_Pre`, `SafeHook_Post` |
| `plugins/Plugins.cpp` | finding, classifying and loading plugins; the `DK2ML_API` table (`Api_*`); per-plugin hook ownership; mod records for the screen | `Plugins_LoadAll`, `LoadPlugin`, `DisableHooksOf` |
| `plugins/OptionCheck.cpp` | `AddOption` validation, shared with symtest's dry run | `Options_Check` |
| `plugins/Events.cpp` | event subscribers and dispatch, the `AddTask` queue | `Events_Subscribe`, `Events_Dispatch`, `Events_Frame`, `Events_AddTask` |
| `plugins/Interfaces.cpp` | plugin-to-plugin function tables | `Interfaces_Publish`, `Interfaces_Get` |
| `plugins/GameHooks.cpp` | the loader's own game hooks (table above), turned into events; calls the menu from the frame tick and GUI load | `GameHooks_Init`, `GameHooks_Install`, `GameHooks_GameState` |
| `gui/GameUi.cpp`/`.h` | every game name the loader itself uses; game version detection | `gameui::Resolve`, `ResolveEvents`, `ResolveKit`, `FindGameVersion` |
| `gui/nativemods/NativeModsButton.cpp` | the main-menu button and screen, merged into the GUI while it loads (its own `MergeItemsFromXML` hook) | `Menu_Init`, `Menu_OnFrame` |
| `gui/GuiKit.cpp` | the GUI kit (`Gui*`), the game GUI event consumer, game window and version | `GuiKit_Init`, `GuiKit_OnGuiLoaded` |
| `gui/GuiThunks.cpp` | thunks for `GuiSetCallback`, input capture state | `Gui_CallbackThunk`, `Gui_SetCapture` |
| `gui/nativemods/NativeModsScreenXml.cpp`/`.h` | builds the "Native mods" screen as game GUI XML (pure) | `screenxml::Build`, `screenxml::MainMenuButton` |
| `gui/nativemods/NativeModsScreen.cpp`/`.h` | the screen at runtime: pages, widget sync, event 219 | `Screen_BuildItems`, `Screen_OnGameEvent` |
| `safety/Consent.cpp` | path canonicalization and classification, plugin/manifest read, fingerprints, the Workshop prompt, default ini | `Consent_Classify`, `Consent_DecideAll` |
| `safety/CrashReport.cpp` | the crash report: stack walk (through post hooks), owning mod, registers, mod list | `CrashReport_Write`, `Crash_Walk` |
| `Loader.h` | every cross-file declaration, grouped per file | |

### Outside `loader/`
| Path | What |
|---|---|
| `include/dk2ml.h` | the plugin API: C ABI, `DK2ML_API` table, events, options, manifest. `DK2ML_API_VERSION` 1 |
| `include/dk2ml.hpp` | optional header-only C++ layer: `Fn`, `Global`, `Field`, `TypeSize`, `Enum`, `ResolveAll`, `On`, `Hook`, typed `Arg`/`SetArg`, `dk2ml::gui` helpers. No ABI of its own |
| `template/` | starting project for plugin authors (CMake, `src/Plugin.cpp`, `mod/`, build/install/package scripts); released as its own zip with the headers, symtest, disasm.ps1 and the cookbook added |
| `docs/cookbook.md` | recipes and pitfalls for plugin authors |
| `docs/ExamplePlugin.cpp` | minimal C plugin, shipped in the template zip's `docs\` (consenttest also uses it as a real plugin DLL) |
| `docs/players.txt` | the player guide, shipped as the loader zip's `README.txt` |
| `tools/SymTest.cpp` | PDB explorer (`--find`, `--types`, `--type`, `--enum`) and plugin dry run. Ships in the template zip's `tools\` |
| `tools/*.ps1` | `build.ps1`, `install.ps1` (dev install), `gen_exports.ps1` (proxy export stubs), `disasm.ps1` (disassembles game functions by name; needs LLVM and a local publics dump in the git-ignored `re/`) |
| `tests/` | tests, all run by `ctest --test-dir build`: hooktest (safe-hook register guarantees), consenttest, screentest, hpptest, eventstest, crashtest, guitest, and the test DLLs they load |
| `.github/workflows/` | `ci.yml` (build and tests on every push and pull request), `release.yml` (Run workflow: builds, tests, packages and publishes a release) |
| `tools/third_party/minhook/` | MinHook 1.3.4, used for the entry-point hook and under every safe hook |

## How plugins relate to the loader
- A plugin is a 64-bit DLL in a mod's `native\` folder (or in `<game>\mods_native\`) that exports
  `int DK2ML_PluginInit(const DK2ML_API* api, const DK2ML_PluginInfo* info)`.
- It does not link against the loader. Everything goes through the `DK2ML_API` function table passed to init.
- It may export a manifest with `DK2ML_PLUGIN_MANIFEST(minApi, name, version, author, url, gameVersion)`. The loader
  reads it without running code and shows it in the log, crash reports and the Native mods screen.
- The loader decides whether it loads: the mod is enabled in the game's Mods menu, the folder is local or an
  approved Workshop item, and `minApi` is not above the loader's API version.
- `DK2ML_PluginInit` runs once, on the main thread, before any game code. Options, event subscriptions and
  interfaces can only be added there, and hooks are normally created there too. Return 0 to stay loaded. A non-zero
  return or a crash disables all of that plugin's hooks and options.
- `info` gives the plugin's DLL path, its folder and its mod folder.
- Many plugins at once:
  - Any number can safe-hook the same function. The loader keeps one MinHook hook per target with a chain: pre
    callbacks in load order, posts reversed.
  - Events have no listener limit, so common moments need no hook at all.
  - `PublishInterface(name, version, table)` during init, `GetInterface` from `PLUGINS_LOADED` on: plugins call each
    other through function tables. A crashed publisher's tables are withdrawn.
  - Two mods with the same plugin file name: only the first loads.
  - The log reports shared hooks and a pre that skips the original under other mods' hooks; the log and the screen
    report clashing dependency DLLs and duplicate hotkeys.

## Hooking into the game: the ways in
From least to most invasive.

| Need | Use |
|---|---|
| Read or write game state | `ResolveSymbol` for globals (returns the variable's address, so a global pointer needs one more dereference), `GetFieldOffset`, `GetTypeSize`, `GetEnumValue` (C++ enumerator names, e.g. `CGAMESTATE_RUNNING`) |
| Call a game function | `ResolveSymbol("Class::Method")`, cast to a function pointer. Pass the decorated name to pick one overload |
| Run code every frame, on state change, on map load, on GUI reload, on window resize | `Subscribe(DK2ML_EVENT_*, fn, user)`. No hook needed |
| React to one of the game's GUI events | `SubscribeGuiEvent(id, fn, user)` (`GUI::Events::eEventType` values). Arrives after the game handled it |
| Change what a game function does | `CreateSafeHook(target, pre, post, user)` then `EnableHook(target)` |
| Add GUI | XML files in the mod's `gui\` folder (appended to the game's GUI), then the GUI kit: `GuiFind`, `GuiShow`, `GuiSetText`, `GuiAddChild`, `GuiSetOrigin`, `GuiClick`, `GuiSetCallback` |
| Keep clicks and keys off the map while your UI is up | `CaptureGameInput(1)` / `CaptureGameInput(0)` |
| Settings | `AddOption` (shown with the game's widgets on the Native mods screen), saved by you under `GetConfigDir()` |
| Get onto the main thread from another thread | `AddTask(fn, user)`: runs at the start of the next frame |

### Safe hooks
- Always use `CreateSafeHook` for game functions, never a plain detour of the plugin's own.
- Reason: the game is built with link-time code generation. Callers keep values in registers that the x64
  calling convention lets a callee overwrite, because the compiler knows the real callee doesn't. A C++ detour
  overwrites them and corrupts the caller.
- A safe hook saves every general and xmm register, calls your `pre(DK2ML_Regs*, user)` with them, restores them,
  and jumps to the original exactly as the caller set it up.
- `pre` reads and changes arguments (`rcx`, `rdx`, `r8`, `r9`, `xmm0`–`xmm3`, `stack[5]`…, or `DK2ML_Arg` /
  `dk2ml::Arg<T>`). Returning `DK2ML_SKIP_ORIGINAL` returns to the caller with `rax`/`xmm0` as the result.
- `post` runs after the original and can change the result. `scratch[]` carries values from pre to post.
- Callbacks run under `__try`. A crash restores the registers and turns all of that plugin's hooks into pass-through.
- Hooks start disabled. `EnableHook`/`DisableHook` act on the calling plugin's callbacks only.

### Minimal plugin (C++ layer)
```cpp
#include "dk2ml.hpp"

DK2ML_PLUGIN_MANIFEST(1, "My Plugin", "1.0.0", "You", "", 112);

namespace {
const DK2ML_API* g_api = nullptr;
dk2ml::Fn<void(void* gameClient, int dt)> UpdateCamera{"GameClient::UpdateCamera"};
dk2ml::Field<float> Camera_m_fov{"Camera", "m_fov"};
dk2ml::Enum Running{"GameClient::eCGameState", "CGAMESTATE_RUNNING"};

int UpdateCameraPre(DK2ML_Regs* regs, void*)
{
    int dt = dk2ml::Arg<int>(regs, 1); // argument 0 is `this`
    (void)dt;
    return DK2ML_CALL_ORIGINAL;
}

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
    if (!dk2ml::On(api, DK2ML_EVENT_STATE_CHANGED, OnStateChanged) || !dk2ml::Hook(api, UpdateCamera, UpdateCameraPre))
        return 2;
    return 0;
}
```
The full version, with settings and a hotkey, is [`template/src/Plugin.cpp`](../template/src/Plugin.cpp).
The C-only version is [`ExamplePlugin.cpp`](ExamplePlugin.cpp).

### Tools for plugin authors
- `symtest.exe "<game>" --find "GameClient::*"`: functions and globals, with decorated names for overloads.
- `--types "*Camera*"`, `--type Camera`, `--enum GameClient::eCGameState`: types, layouts, enum values.
- `symtest.exe "<game>" my_plugin.dll`: runs the plugin's init against the real PDB with stubbed hooks. Must print
  `plugin init returned 0`. Exit code 100 means a call the real loader would refuse.
- `tools\disasm.ps1 -Names '<name>'`: disassembly, to check how callers use a function before hooking it.

## Limits
- A plugin's own threads are not crash-contained.
- Changes a plugin makes to the game around the API (its own patches, threads, window hooks) aren't tracked.
- A crash below a post-hooked function can't be unwound past the post stub: no game crash dump and no loader report
  for it.
- Consent means trusting the author: nothing checks what a mod's code does.
- Names that a game update removes make the plugin's init fail cleanly. Its Native mods page lists the first three
  missing names.
