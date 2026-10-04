# dk2ml cookbook

Recipes and common pitfalls for Door Kickers 2 native mods (dk2ml plugins). Start from the template
(`DK2-NativeModTemplate-v<version>.zip` on the loader's release page). Every name here is resolved from
`DoorKickers2.pdb` at runtime, so a plugin keeps working across game updates as long as the names exist.

The examples use `dk2ml.hpp` (the C++ layer). The C API in `dk2ml.h` can do everything too.

- [Workflow](#workflow)
- [Finding things in the game](#finding-things-in-the-game)
- [Reading game state](#reading-game-state)
- [Calling game functions](#calling-game-functions)
- [Hooking](#hooking)
- [Every frame, mission start, hotkeys](#every-frame-mission-start-hotkeys) (events)
- [Settings](#settings)
- [GUI: buttons in the game's menus](#gui-buttons-in-the-games-menus)
- [ImGui windows](#imgui-windows)
- [Memory, threads, co-op](#memory-threads-co-op)
- [Living with other mods](#living-with-other-mods)
- [Debugging](#debugging)
- [After a game update](#after-a-game-update)
- [Publishing checklist](#publishing-checklist)

## Workflow
1. `.\build.ps1` → `build\<name>.dll`.
2. `.\install.ps1` (game closed): dry run, then copy to `%LOCALAPPDATA%\KillHouseGames\DoorKickers2\mods_upload\<name>\`. Your own mod folders load without a permission prompt. Enable the mod once in the Mods menu.
3. Start the game and read `<game folder>\dk2ml.log`: your `Log` lines are prefixed with your DLL's name. The previous session's log is `dk2ml.prev.log`.

The **dry run** (`symtest.exe "<game folder>" <your.dll>`) runs your `DK2ML_PluginInit` against the real PDB with nothing hooked. A missing name, a bad `AddOption` or a hook the loader would refuse shows up there, not in the game. Exit code 0 means clean, 100 means init returned 0 but something would be refused (`PROBLEM` lines), and anything else is your init's return value.

## Finding things in the game
`symtest.exe` reads the PDB for you (no other tools needed):
```powershell
symtest.exe "<game>" --find "GameClient::*"          # functions and globals; * and ? wildcards
symtest.exe "<game>" --types "*Trooper*"             # type names (and "enum X")
symtest.exe "<game>" --type Camera                   # fields with offsets, sizes, types; base classes; bitfields
symtest.exe "<game>" --enum GameClient::eCGameState  # enumerator names and values
```
`--find` flags two things worth knowing before you hook:
- **OVERLOAD**: several functions share the name. `ResolveSymbol` with the plain name returns one of them; pass the decorated name it prints (`?FindChild@Item@GUI@@QEAAPEAV12@PEBD@Z`) to get the one you mean.
- **FOLDED**: the linker merged identical functions into one body (common for tiny getters and empty virtuals). A hook there runs for every one of them; check `this` in the callback.

To see what a function does, disassemble it. `tools\disasm.ps1` (in the template zip or the loader repository) annotates calls with names; it needs LLVM (`llvm-objdump`, `llvm-pdbutil`).

## Reading game state
```cpp
dk2ml::Global<void*> g_pGameClient{"g_pGameClient"};
dk2ml::Field<int> GameClient_m_state{"GameClient", "m_state"};
dk2ml::Enum StateRunning{"GameClient::eCGameState", "CGAMESTATE_RUNNING"};

void* client = *g_pGameClient;                 // may be null early on
bool inMission = client && GameClient_m_state(client) == StateRunning.Get();
```
- **A global's address is the variable, not its value.** `ResolveSymbol("g_pGameClient")` returns where the pointer is stored (`GameClient**`); dereference it every time you need the object, since the game can replace it.
- **Use enum names, not numbers.** `CGAMESTATE_MAIN_MENU` is 3 and `CGAMESTATE_RUNNING` is 8 in build 112, and the numbers change if the game adds states.
- **Nested structs:** add the offsets: `GetFieldOffset("GameClient", "m_freelook") + GetFieldOffset("GameClient::sFreelook", "bEnabled")`.
- **Bitfields:** `GetFieldOffset` gives the storage unit's offset; the log (and `--type`) give the bits. Shift and mask yourself.
- **Your struct types must match the game's layout** (e.g. `struct Vector3 { float x, y, z; };`). Check sizes with `--type`. Never map game memory onto `std::` types (see Memory).

## Calling game functions
```cpp
// void Camera::UpdateViewMatrix(): `this` is the first argument
dk2ml::Fn<void(void* camera)> UpdateViewMatrix{"Camera::UpdateViewMatrix"};
// Vector3 GameClient::ConvertScreenToMapCoords(float x, float y) const: a class returned by value comes back through
// a hidden pointer that goes right after `this`, and the function returns that pointer
dk2ml::Fn<Vector3*(const void* client, Vector3* result, float x, float y)> ScreenToMap{"GameClient::ConvertScreenToMapCoords"};

Vector3 at;
ScreenToMap(client, &at, mouseX, mouseY);
```
The x64 calling convention, as it applies here:
- the first four arguments go in `rcx`, `rdx`, `r8`, `r9`, or in `xmm0`–`xmm3` for floats and doubles, by position; the rest go on the stack;
- a struct bigger than 8 bytes passed by value (like `Vector3`) is passed as a pointer to a copy: declare it as `const Vector3*`;
- a class returned by value uses a hidden result pointer as the first argument (second for member functions).

Call game functions only from code the game runs on its main thread (your hooks of main-thread functions, the frame tick), never from threads of your own: the game isn't thread-safe.

## Hooking
Always use **safe hooks** (`CreateSafeHook`, or `dk2ml::Hook`). The game is built with link-time code generation, and its callers keep values in registers that a normal C++ detour overwrites. That silently corrupts the caller: in Free Camera, a pass-through detour on `Camera::SetDefaults` corrupted the shadow map. A safe hook saves and restores every register around your callbacks.
```cpp
dk2ml::Fn<void(void* camera)> SetDefaults{"Camera::SetDefaults"};

int Pre(DK2ML_Regs* regs, void*)
{
    void* camera = dk2ml::Arg<void*>(regs, 0);
    regs->scratch[0] = reinterpret_cast<uint64_t>(camera); // handed to Post for this call
    return DK2ML_CALL_ORIGINAL;                            // or set a result and return DK2ML_SKIP_ORIGINAL
}
void Post(DK2ML_Regs* regs, void*) { /* runs after the original; dk2ml::Result<T>(regs) is its result */ }

dk2ml::Hook(api, SetDefaults, Pre, Post);  // in DK2ML_PluginInit: created and enabled
```
- **Arguments** are registers: `dk2ml::Arg<T>(regs, n)` and `SetArg` find them by position and type, and `Result`/`SetResult` handle the result. Read arguments in the pre: by the post, the registers hold whatever the function left.
- **Filter by object.** Many functions serve more than one caller: `Camera::SetDefaults` runs for every temporary render camera, not just the view. Compare `this` with the object you care about.
- **Other plugins may hook the same function**, any number of them. Pres run in load order and posts in reverse. A pre that skips the original ends the chain, and the posts before it still run. Keep pre/post pairs self-contained, with state in `scratch`. See [Living with other mods](#living-with-other-mods).
- **Crash containment:** if your callback crashes, the loader catches it, switches off all your hooks and options for the session, and the game carries on. From then on `CreateSafeHook`, `EnableHook` and `GuiSetCallback` refuse your plugin, even from your own threads. Your mod's page on the Native mods screen shows the crash, so players can report it.
  - Not contained: your own threads, damage done before the crash, and a crash inside a game function your callback called while a post-hooked call is between them on the stack.
- **You can be switched off at any moment, without notice.** That happens after a crash, and when the player disables your mod in the Mods menu. The game applies that menu without restarting. The loader switches the mod's plugins off at the next GUI load, and the game drops your GUI files. Don't leave game state that only your code can undo. Prefer changes the game resets by itself, for example on the next map load. Your threads keep running, so give them a way to stop on their own.

## Every frame, mission start, hotkeys
Use the loader's **events** for these. The loader hooks the game once and calls every plugin that subscribed, so you need no hook of your own and there's no limit on listeners.
```cpp
dk2ml::Enum StateRunning{"GameClient::eCGameState", "CGAMESTATE_RUNNING"};

void OnState(const DK2ML_Event* e, void*)
{
    if (e->newState == StateRunning.Get()) { /* a mission is running */ }
    else if (e->oldState == StateRunning.Get()) { /* it ended (menu, loading, restart) */ }
}

// in DK2ML_PluginInit (subscriptions only work there):
dk2ml::On(api, DK2ML_EVENT_STATE_CHANGED, OnState);
dk2ml::On(api, DK2ML_EVENT_FRAME, [](const DK2ML_Event*, void*) { /* every frame */ });
```
| Event | When |
|---|---|
| `DK2ML_EVENT_FRAME` | Every frame on the main thread, except while a random map generates. ImGui's frame is open, so you can draw windows. |
| `DK2ML_EVENT_STATE_CHANGED` | `GameClient::m_state` changed (`oldState` → `newState`, `-1` = no GameClient yet). Compare by name, e.g. `CGAMESTATE_RUNNING`, `CGAMESTATE_MAIN_MENU`. |
| `DK2ML_EVENT_MAP_LOADED` | A map is being set up: every mission start and restart, just before it runs. |
| `DK2ML_EVENT_GUI_LOADED` | The game (re)built its GUI: items you found before are gone, so find and attach yours again. |
| `DK2ML_EVENT_PLUGINS_LOADED` | Once, right after every plugin's init, before any game code runs. Look up other plugins' interfaces here (see [Living with other mods](#living-with-other-mods)). |
| `DK2ML_EVENT_WINDOW_RESIZED` | The game window's size changed (`width`, `height`: its client size). `api->GetGameWindow()` is the window. |
| `DK2ML_EVENT_GUI_EVENT` | One of the game's GUI events, subscribed by id with `SubscribeGuiEvent`: see [GUI](#gui-buttons-in-the-games-menus). |

`api->GetGameState()` gives the current state at any time.
- **Placing troops:** `GameGUI::m_deploySlots` holds elements exactly while troops are being placed (the game's own deploy code relies on it).
- **Hotkeys:** `GetAsyncKeyState` sees keys pressed in any window, so check `api->IsGameFocused()` first, and act on the press (down now, up last frame), not while it's held. Skip them while a game menu is open: `api->IsGameMenuOpen()`. The API has no way to tell that the player is typing (chat, names).
- **Key options:** `DK2ML_OPTION_KEY` holds a virtual-key code and lets players rebind it on the Native mods screen.

## Settings
- Declare them with `AddOption` in `DK2ML_PluginInit`, and the loader shows them on your page of the main menu's Native mods screen with the game's own widgets. You keep the values: it reads `*value` when the screen opens, writes it on change, then calls `onChange`. Save there.
- **Save into `api->GetConfigDir()`**, never into your mod folder: Steam replaces a Workshop mod's folder on every update. It's named after your DLL, so a distinctive DLL name matters.
- **Shipping defaults:** files you put in `native\` are part of what Workshop players approve, and changing them triggers a new permission prompt. Copy a defaults file to the config dir on first start, then only ever write the copy.
- The Native mods screen exists on the main menu only. Free Camera adds its own ImGui window for changing settings during a mission.

## GUI: buttons in the game's menus
- Every `.xml` in your mod's `gui\` folder is loaded, and its top-level items are added to the GUI. **Never replace `hud.xml` or other game files**: other mods change them too. Prefix your item names (`#mymod_button`).
- **The GUI kit** (`dk2ml::gui::` in `dk2ml.hpp`) does the common things with the game's own functions and actions, and the loader resolves the layouts they need: `Find`, `Parent`, `Name`, `Children`, `IsShown`/`Show`, `SetText`, `AddChild`, `SetOrigin`, `Click`, `OnAction`. Main thread only; items are gone after `GUI_LOADED`, so find them again.
- **Putting your button into a game menu:** an item has one parent. Each frame, if your item isn't in the target menu yet, move it with `AddChild` (the game's `ACTION_ADD_CHILD`, what `<Action type="AddChild">` does in XML). Free Camera's `menu.cpp` (its Esc-menu button) does this:
  ```cpp
  void* button = dk2ml::gui::Find(api, "#mymod_button");
  void* menu = dk2ml::gui::Find(api, "Menu_Ingame");
  void* row = menu ? dk2ml::gui::Find(api, "Extra Buttons", menu) : nullptr; // look it up inside the menu
  if (button && row && dk2ml::gui::Parent(api, button) != row && dk2ml::gui::AddChild(api, row, button))
      dk2ml::gui::Show(api, button);
  ```
- **Reacting to a click** (or hover, scroll, drag): give the item an `<Action type="Callback" target="#mymod_button"/>` under that event (`OnClick`, `OnHover`, `OnScrollDown`, …; the game wants an existing item as the target), and point it at your function after GUI_LOADED:
  ```cpp
  dk2ml::Enum EVENT_CLICK{"GUI::Item::eItemEventType", "EVENT_CLICK"};
  dk2ml::gui::OnAction(api, button, EVENT_CLICK.Get(), [](void* item, float x, float y, void*) { /* clicked */ });
  ```
  It runs crash-contained, gets the item (for a cloned item, the clone) and the cursor in GUI space (+y up). Cloning copies callbacks, so set them on a template before you clone it.
- **The game's own GUI events** (`GUI::Events::eEventType`: a screen opened or closed, a button's `TriggerEvent`, …) arrive without a hook: `dk2ml::OnGuiEvent(api, GUI_GAME_CUSTOMIZE_OPENED.Get(), fn)` from init. They arrive after the game handled them and can't be blocked, because the game calls every listener (`EventSystem::TriggerEvent`, newest listener first). To stop the game's handling (e.g. a BACK that should close your panel instead of the screen), safe-hook the handler, e.g. `GameGUI::Customize_OnEvent`.
- XML rules: action targets must name existing items (they're checked at load), `type="None"` is rejected, and only event names from `GUI::Events::eEventType` work in `TriggerEvent`. The game's `data\gui\_config.xml` documents the GUI elements and every action type. GUI XML errors appear in `%LOCALAPPDATA%\KillHouseGames\DoorKickers2\log.txt`.

## ImGui windows
The game links Dear ImGui and renders it every frame, so a plugin can draw ImGui windows:
- resolve the functions by name: `ImGui::Begin`, `ImGui::End`, `ImGui::Checkbox`, `ImGui::SliderScalar`, `ImGui::ButtonEx`, `ImGui::Text`, …
- call them from a `DK2ML_EVENT_FRAME` callback.
- **Some functions don't exist:** they were inlined away (`SetNextWindowPos`, `CollapsingHeader`, `PopID`, …). Check with `--find "ImGui::*"`, and use the underlying function where there is one (`ButtonEx` for `Button`, `SliderScalar` for `SliderFloat`, `SeparatorEx` for `Separator`).
- **Flags by name:** `ImGuiWindowFlags_` and the other flag enums are in the PDB (`--enum ImGuiWindowFlags_`). Get them with `dk2ml::Enum`, not hardcoded numbers, which change between ImGui versions.
- **Clicks on your window also reach the map** unless the game thinks a menu is open. Call `api->CaptureGameInput(1)` while your window is open and `0` when it closes: the loader then answers `GameGUI::IsAnyMenuOpened` with true for everyone. `api->IsGameMenuOpen()` still tells you about the game's own menus.
- Match the game's ImGui types exactly (`ImVec2 { float x, y; }`). Pass by reference where the real signature does (`ButtonEx(const char*, const ImVec2&, int)` takes a pointer).

## Memory, threads, co-op
- **Two heaps:** your plugin has its own C runtime (static CRT), and the game has its own allocators (`malloc`/`free` and an `operator new(size_t, _HeapManager&, int)` of its own). Never free memory the game allocated, and never give the game memory it will free later. Where the game keeps a buffer you hand it, keep yours alive for the rest of the process.
- **No `std::` types over game memory.** The game's containers are its own (`List<T>`, `LinkedList<T>`; look at them with `--type`), and a Debug build of your plugin lays out `std::vector`/`std::string` differently anyway. Build Release.
- **Threads:** hook callbacks run on whatever thread the game calls the function on. `ImGui::Render` and the game loop run on the main thread. If you start threads, don't touch game state from them. The loader can't contain crashes on your threads.
- **Back to the main thread:** `api->AddTask(fn, user)` (or `dk2ml::Post(api, fn, user)`) runs `fn` on the main thread at the start of the next frame, under crash containment, from any thread. It's how a worker thread (a download, a file watcher) hands its result to code that may call the game. Tasks run in order; one added while tasks run waits for the next frame, and nothing runs while a random map generates. Keep `user` alive until the task has run.
- **Co-op:** Door Kickers 2 has online co-op. A mod that changes gameplay on one machine only can desync a session. Keep gameplay changes to single player, or make sure every player runs the same mod.

## Living with other mods
Players run several native mods at once. The loader keeps them apart mechanically: each has its own callbacks in the hook chains, its own crash containment, settings folder and page. It can't make them agree. What helps:
- **Prefer events to hooks** for frames, game state, map loads and GUI reloads: no chains, no order to worry about.
- **Skip the original only when you must.** When your pre returns `DK2ML_SKIP_ORIGINAL`, plugins after yours on that function don't run for the call. The log says once who skipped whom. Which plugin runs first is the order of the mods in the Mods menu, which neither of you controls.
- **Don't fight over state.** If two mods write the same field every frame (both setting the camera angle), the last writer wins and the result jitters. Write only when your feature is active. If the value isn't what you last wrote, another mod is using it, so consider backing off.
- **Give dependency DLLs unique names, or link them in.** Windows loads a DLL name once per process. If two mods ship different `helper.dll`s, one of them runs against the other's copy. The loader warns about it, in `dk2ml.log` and on both mods' pages, but can't fix it.
- **Prefix GUI item names** (`#mymod_...`), and don't replace the game's GUI files.
- **Keys:** the Native mods screen shows "(also <mod>)" when two mods' key options are bound to the same key. Pick unusual defaults.
- **Go through the loader.** Use events and safe hooks, never your own patches. A direct code patch can break other mods' hooks on that function. Anything you change around the loader (code bytes, import or vtable entries, a subclassed game window) keeps running if your plugin crashes or the player disables your mod, and nothing tells players about it.
- **Plugin DLL names must be unique too:** a second plugin with the same file name isn't loaded, and the two would share a settings folder.
- **Offer functions to other plugins** with an interface instead of having them hook yours or read your memory. Publish a table of function pointers by name in init; others look it up from `DK2ML_EVENT_PLUGINS_LOADED` on:
  ```cpp
  // in your public header, shared with other mods: append fields only, raise the version when you do
  struct MyModApi { uint32_t structSize; bool (*IsActive)(); void (*SetZoom)(float); };
  // your plugin, in DK2ML_PluginInit
  static const MyModApi kApi = {sizeof(MyModApi), IsActive, SetZoom};
  dk2ml::Publish(api, "author.mymod.Api", 1, &kApi);
  // another plugin, from PLUGINS_LOADED on (always NULL during init, so load order never matters)
  if (auto* m = dk2ml::Get<MyModApi>(api, "author.mymod.Api", 1)) m->SetZoom(2.0f);
  ```
  A name is taken once (the second publisher is refused and logged). Look the table up when you need it instead of keeping the pointer: once the publisher is switched off (its init failed, or it crashed), `GetInterface` returns NULL. A crash inside the other plugin's function counts as a crash of your callback. `dk2ml.log` lists who offers which interface.

## Debugging
- **Logs:** `api->Log` writes to `dk2ml.log` and to the debugger output, so Sysinternals DebugView shows it live. Don't log every frame: each line is flushed to disk.
- **A debugger:** build with symbols (`.\build.ps1 -Config RelWithDebInfo`), start the game, then in Visual Studio use Debug > Attach to Process > `DoorKickers2.exe`. Breakpoints in your plugin work once it's loaded, and the game's own functions have names, thanks to its PDB.
- **When things go wrong:**
  - A crash in a safe-hook callback is logged with its exception code, and your plugin is switched off for the session.
  - **Crash reports:** when the game crashes with an unhandled exception, the loader writes `dk2ml-crash-<date>-<time>.log` in the game folder just before the game's own crash dump. The 10 newest are kept.
    - Contents: what crashed and where, every mod whose code is on the stack, the stack (game functions named from the PDB, hooked functions marked with their hooks' owners), the registers, and the mods with their manifests.
    - Plugin frames show as `yours.dll+0x1234`. Look the offset up in the PDB of the build that crashed.
    - A crash below a function that some plugin post-hooks can't be traced to the game's crash handler, so it gets no report and no game dump.
  - `enabled=0` under `[loader]` in `dk2ml.ini` (game folder) starts the game without native mods.
- **The Native mods screen** (main menu) shows whether your plugin loaded, and why not.

## After a game update
1. Run the dry run against the updated game. It prints each missing name (`dk2ml::ResolveAll` logs every one).
2. Find what they became with `--find` / `--type` / `--enum`.
3. A name can stay while its signature changes. If a hooked or called function behaves oddly after an update, compare its disassembly.
4. Update `gameVersion` in `mod.xml`, rebuild, upload. Workshop players are asked for permission again, because the code changed.

## Publishing checklist
- Release build, a clean dry run (exit code 0), tested in game from `mods_upload`.
- The mod folder: `mod.xml` (title, description, `gameVersion`), `native\<name>.dll` and its dependencies, an optional `gui\`, and usually `mod_image.jpg`.
- The Workshop page says the mod needs the **Door Kickers 2 Native Mod Loader** and links it. Without the loader, a native mod silently does nothing.
- **Add a manifest:**
  ```cpp
  DK2ML_PLUGIN_MANIFEST(1, "My Plugin", "1.0.0", "You", "https://steamcommunity.com/...", 112);
  ```
  The arguments are the plugin API version you need (`DK2ML_API_VERSION` of the `dk2ml.h` you build with), name, version, author, a link (may be `""`), and the game version you made it for (as in `mod.xml`). The loader reads it from the DLL without running it, and shows it on your Native mods page, in crash reports and in `dk2ml.log`. A loader with a lower API version doesn't load your plugin and says it needs a newer loader. The dry run prints it and flags a manifest the loader would refuse.
- Upload from the game's Mods menu (copy `dist\<name>` to `mods_upload\<name>` first).
