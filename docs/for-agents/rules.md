# Rules

Hard rules for dk2ml plugin code. Cite them by number. Each has its reason, so edge cases can be judged.

## Hooking and game access
- **R1. MUST hook game functions only with `CreateSafeHook` (or `dk2ml::Hook`). NEVER use a plain C++ detour, MinHook, Detours or byte patches.**
  The game is built with link-time code generation, and callers keep live values in ABI-volatile registers across calls. A plain detour clobbers them and silently corrupts the caller. A pass-through detour on `Camera::SetDefaults` corrupted the shadow map. Patches also break other plugins' hooks and keep running after the loader switches the plugin off.
- **R2. NEVER hardcode addresses, offsets, type sizes or enum values.** Resolve them by name: `ResolveSymbol`, `GetFieldOffset`, `GetTypeSize`, `GetEnumValue`, or the `dk2ml.hpp` bindings. Addresses and offsets move between game builds, and enum values change when the game adds enumerators.
- **R3. MUST check every game name with `symtest` before writing code that uses it.** Names are never guessed. See [symbols.md](symbols.md). A wrong name fails init in game. The dry run catches it, but checking first saves a cycle.
- **R4. MUST declare `dk2ml::Fn`/`Global`/`Field`/`TypeSize`/`Enum` at namespace scope or as static members, and call `dk2ml::ResolveAll(api)` in init.** Bindings register themselves in their constructors. A local or temporary binding is never resolved.
- **R5. MUST call game functions, the GUI kit and ImGui only on the main thread:** from hook callbacks of main-thread functions, events, tasks or GUI callbacks. The game isn't thread-safe. Use `AddTask` to get onto the main thread from another thread.
- **R6. MUST filter hook callbacks by object (`this`, argument 0) when the function serves several callers or `symtest --find` marks it `FOLDED`.** A hook runs for every caller and for every function folded into the same body.
- **R7. MUST match the game's layouts in your own structs** (check with `symtest --type`). **NEVER map `std::` types onto game memory.** The game's containers are its own (`List<T>`, `LinkedList<T>`), and `std::` layouts differ by build configuration.
- **R8. NEVER free memory the game allocated, and NEVER hand the game memory it will free.** The plugin has its own CRT heap, and the game has its own allocators (its own `malloc`/`free` and an `operator new(size_t, _HeapManager&, int)`). A buffer the game keeps must stay alive for the rest of the process.
- **R9. Prefer the least invasive tool: a loader event, then GUI XML and the GUI kit, then a safe hook.** Prefer calling the game's own functions to writing game state by hand. Events have no listener limit and no chain order to worry about. See [api-map.md](api-map.md#pick-the-least-invasive-way).
- **R10. Skip the original (`DK2ML_SKIP_ORIGINAL`) only when the task requires it.** Plugins later in the chain don't run for that call.

## Init and lifetime
- **R11. MUST call `AddOption`, `Subscribe`, `SubscribeGuiEvent` and `PublishInterface` only inside `DK2ML_PluginInit`.** Later calls return `DK2ML_ERROR`. Create hooks there too.
- **R12. NEVER use the GUI kit or game objects during init.** No GUI and no game objects exist yet (`*g_pGameClient` is null). Find GUI items from `DK2ML_EVENT_GUI_LOADED` on, and find them again after every `GUI_LOADED`.
- **R13. MUST return non-zero from init when a required step fails** (`ResolveAll`, a hook, a subscription). The loader then switches the plugin off cleanly.
- **R14. MUST NOT leave game state that only the plugin can undo.** The plugin can be switched off at any time, after a crash or when the player disables the mod mid-session. Prefer changes the game resets by itself, for example on the next map load.
- **R15. Data passed to a game function from a hook (pointers to arguments) MUST outlive the callback:** static storage, not the stack.

## Files, names, GUI
- **R16. MUST save settings and other written files under `api->GetConfigDir()`.** NEVER write to the mod folder (`info->modDir`, `info->pluginDir`). Steam replaces a Workshop mod's folder on every update. Changing files in `native\` also triggers a new permission prompt for players.
- **R17. MUST give the project, and so the DLL, a distinctive name.** Change `project(my_plugin ...)` in `CMakeLists.txt`. The DLL name is the settings folder name. A second plugin with the same file name isn't loaded.
- **R18. MUST prefix GUI item names (`#mymod_...`).** NEVER replace `hud.xml` or other game GUI files. Add new `.xml` files in the mod's `gui\` folder. Other mods share the GUI namespace and edit the same game files.
- **R19. Dependency DLLs need unique file names, or link them statically.** Windows loads each DLL name once per process.
- **R20. Gameplay changes can desync online co-op.** Keep them to single player, or state that every player needs the mod.

## Build and environment
- **R21. MUST ship Release builds.** Debug changes `std::` layouts and slows every frame. `.\tools\build.ps1` defaults to Release.
- **R22. MUST pass the dry run before calling a change done:** `plugin init returned 0` and exit code 0. See [workflow.md](workflow.md).
- **R23. NEVER launch, kill or attach to `DoorKickers2.exe`.** In-game testing is the human's. `install.ps1` refuses to run while the game runs. Ask the human to close it.
- **R24. NEVER publish or upload** (Steam Workshop, releases). Tell the human what to publish.
- **R25. NEVER commit game-derived material** to the plugin's repository: PDB dumps (`publics.txt`, type dumps), disassembly output, game assets. Keep them local and git-ignored.
- **R26. MUST NOT change `dk2ml\dk2ml.h` or `dk2ml.hpp`.** They describe the loader's ABI. Update them only by copying newer versions from a loader release.
