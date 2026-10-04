# dk2ml: Door Kickers 2 native mod loader
dk2ml lets mods for *Door Kickers 2: Task Force North* ship DLLs, making much more complex mods possible.

### WHY?
Door Kickers 2's built-in modding is limited. I've become slightly addicted to the game but found some aspects deeply annoying, and these couldn't be fixed without external code.

### HOW?
The loader takes the place of `dbghelp.dll`, a DLL the game loads at startup, and forwards the game's own calls to the real one. It then connects mod DLLs to the game's code. Game functions, globals, struct fields and enums are found by name in the `DoorKickers2.pdb` the game ships with, so mods don't depend on addresses that move between game builds.

The loader changes nothing about how the game plays and modifies no game files. It hooks into relevant game functions in memory and passes them to mods as events.

Native mods are distributed through the Steam Workshop like any other mod. Their code runs only after the player
enables the mod in the game and allows it in a permission prompt (after restart). The prompt comes back whenever the mod's code changes.

------

> [!WARNING]
> DLLs loaded through the Steam Workshop (or installed manually) are not limited to the modloader's API. **They can run any code and can make your machine vulnerable.** You give someone else's code the same access to your system that the game has. That is the cost for this level of moddability.
>
> The loader asks before running a Workshop mod's code, and again after every update. It does nothing more. Reading the mod's source code **and** decompiling the shipped DLLs are good steps to take, but there's no realistic way to guarantee the quality, reliability or safety of random mod DLLs.
>
> ⚠️ **DO NOT RUN MODS FROM SOURCES YOU DO NOT TRUST.** ⚠️

> [!NOTE]
> AI was used both as an assistant in research and to generate most of the code. Claude Code, Opus 5.5.

## INSTALLATION STEPS (for players)
1. Download `DK2-NativeModLoader` from [Releases](../../releases) and close the game.
2. Copy `dk2ml.dll` and `dbghelp.dll` into the root folder of Door Kickers 2. To get there, in Steam, right-click Door Kickers 2 > Manage > Browse local files. You're looking for the folder containing the game's `.exe`.
3. Download mods that require the Modloader from the Steam Workshop.
4. Open the game, enable the mod you downloaded.
5. Manually quit the game and restart it through Steam. Restarting via the prompt in the mod page will not load DLLs.
6. You should see a Windows prompt telling you external code is being loaded. Accept it and play. Profit.

**Native mods screen.** The main menu gets a **Native mods** button on the bottom right, above "Send Feedback". There you'll see all the mods registered with the Modloader; they each have their own tab with settings (if they have any settings to change). 

## License
Overall, just don't be a dick.
MIT (see [LICENSE](LICENSE)). Includes [MinHook](https://github.com/TsudaKageyu/minhook) (BSD-2-Clause).
Door Kickers 2 is © KillHouse Games; this project isn't affiliated with them.
