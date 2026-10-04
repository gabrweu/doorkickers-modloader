# dk2ml
dk2ml allows modders to load code through DLLs into Door Kickers 2 to enable much more complex mods than otherwise possible.

### WHY?
Default capabilities of mods in Door Kickers 2 are pretty limited. And while I'm a big fan of the game, there are some deep annoyances that seemed easily fixable with more in-depth modding.

### **HOW?**
The modloader stands in for a .dll file the game loads (right now `dbghelp.dll`; could change in the future) and passes the game's own calls through to the original.

It then acts as a bridge between mod DLLs and the game's code, using the PDB file the game ships with to find game functions by name. On its own, the modloader doesn't change how the game plays, it hooks relevant game functions in memory (no game files are modified) and surfaces them as events mods can listen to. Mods can also hook and call game functions themselves.

Mod DLLs are shipped through the Steam Workshop like any other mod. Their code only runs after the player enables the mod in-game and allows it in a permission prompt, which asks again whenever the mod's code changes.

> [!WARNING]
> Please note that third party DLLs loaded through the Steam Workshop (or downloaded manually) are not bound to the modloader's API. **They can run whatever code they want and introduce serious security vulnerabilities into your machine**. You are giving someone else's code access to your whole system, just like the game has. Such is the price for this level of moddability.
>
> The modloader takes _some_ security measures, and having access to the mod's source code **and** decompiling the shipped DLLs are good steps to take.
> But there's simply no realistic way to ensure the quality, reliability and safety of random mod DLLs.
>
> ⚠️ Simply **DO NOT RUN MODS FROM SOURCES YOU DO NOT TRUST**. ⚠️
