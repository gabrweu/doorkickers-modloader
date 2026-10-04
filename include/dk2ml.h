/*
 * dk2ml.h - Door Kickers 2 native mod loader, plugin API.
 *
 * A plugin is a 64-bit DLL in one of:
 *   <enabled mod folder>\native\*.dll   loaded while the mod is enabled in the in-game Mods menu
 *   <game folder>\mods_native\*.dll     always loaded
 * Only DLLs that export DK2ML_PluginInit are plugins. Other DLLs in the folder are dependencies, and Windows finds them
 * in the plugin's folder. If two enabled mods have a plugin with the same file name, only the first loads.
 *
 * Required export:
 *   int DK2ML_PluginInit(const DK2ML_API* api, const DK2ML_PluginInfo* info);   // 0 = success
 *
 * DK2ML_PluginInit runs on the game's main thread before any game code, so no game objects exist yet. Install hooks
 * there. Game functions and types are resolved by name from DoorKickers2.pdb, which ships with the game.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L)
#define DK2ML_INLINE static inline
#else
#define DK2ML_INLINE static __inline
#endif

#define DK2ML_API_VERSION 1

typedef enum DK2ML_Status {
    DK2ML_OK = 0,
    DK2ML_ERROR = 1,
} DK2ML_Status;

/* --- safe hooks ---
 * The game is built with link-time code generation, so callers keep values in registers that the x64 calling
 * convention marks volatile (xmm2-5, r8-r11, ...). A plain C/C++ detour overwrites them and corrupts the caller. A safe
 * hook saves every register, runs the pre callback, restores the registers and runs the original as the caller set it
 * up. The post callback runs after the original returns, with the same care. Use safe hooks for all game functions.
 *
 * Several plugins can hook one function. Pre callbacks run in creation order, post callbacks in reverse. A pre that
 * returns DK2ML_SKIP_ORIGINAL ends the chain. Later callbacks then don't run, and the skipper's own post doesn't run
 * either. The posts of earlier callbacks run right away, with the skipper's result. scratch[] is per callback.
 * EnableHook/DisableHook switch only the calling plugin's callbacks. Once a pre has run, its post runs for that call,
 * even if the hook was disabled in between.
 *
 * Nesting: posts are tracked per thread up to 64 hooked calls deep. Deeper than that, callbacks with a post are
 * skipped entirely (pre too), and pre-only callbacks still run.
 *
 * Identical code folding: the linker merged identical functions (small getters, trivial virtuals, ...). A hook on such
 * an address runs for every function folded into it, and the loader logs their names. Check the arguments (e.g. the
 * object in rcx) if that matters.
 *
 * Crash containment: if a callback crashes, the loader catches it and the call continues as if unhooked. All of the
 * plugin's hooks pass through from then on. The same happens if DK2ML_PluginInit crashes or returns non-zero. Not
 * covered: the plugin's own threads, damage done before the crash (e.g. heap corruption), and a crash in a game
 * function the callback calls while a post-hooked call (any plugin's) is between them on the stack. */

typedef struct DK2ML_Xmm {
    uint64_t lo, hi;
} DK2ML_Xmm;

/* Every register on entry to the hooked function (pre), or as the original left it (post). Changes go to the original
 * (pre) or to the caller (post). */
typedef struct DK2ML_Regs {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15, rflags;
    DK2ML_Xmm xmm[16];
    /* pre: stack[0] is the return address and stack[1..4] the home space of the first four arguments. stack[5] is the
     * fifth argument (DK2ML_Arg n = 4), stack[6] the sixth, and so on. A float is its bit pattern in the low 32 bits. */
    uint64_t* stack;
    /* free for the callback; what pre writes here, post gets for the same call */
    uint64_t scratch[4];
} DK2ML_Regs;

#define DK2ML_CALL_ORIGINAL 0
#define DK2ML_SKIP_ORIGINAL 1 /* return to the caller with the registers in DK2ML_Regs (rax/xmm0 = result) */

typedef int (*DK2ML_PreFn)(DK2ML_Regs* regs, void* user);
typedef void (*DK2ML_PostFn)(DK2ML_Regs* regs, void* user);

/* Argument n (0 = first) in a pre callback. A member function's `this` is argument 0. Integer and pointer arguments
 * 0-3 are in rcx, rdx, r8 and r9. Float and double arguments 0-3 are in xmm0-3 (read them with DK2ML_ArgFloat or
 * DK2ML_ArgDouble). Arguments 4 and later are on the stack. For arguments under 64 bits only the low bytes are defined,
 * so cast: (int)DK2ML_Arg(r, 1), or (uint8_t)DK2ML_Arg(r, 2) != 0 for a bool. dk2ml.hpp's dk2ml::Arg<T> does this. */
DK2ML_INLINE uint64_t DK2ML_Arg(const DK2ML_Regs* r, int n)
{
    switch (n) {
    case 0: return r->rcx;
    case 1: return r->rdx;
    case 2: return r->r8;
    case 3: return r->r9;
    default: return r->stack[n + 1];
    }
}

/* Sets argument n for the original (pre callbacks). Stack arguments are written in place. */
DK2ML_INLINE void DK2ML_SetArg(DK2ML_Regs* r, int n, uint64_t value)
{
    switch (n) {
    case 0: r->rcx = value; break;
    case 1: r->rdx = value; break;
    case 2: r->r8 = value; break;
    case 3: r->r9 = value; break;
    default: r->stack[n + 1] = value; break;
    }
}

DK2ML_INLINE float DK2ML_ArgFloat(const DK2ML_Regs* r, int n)
{
    uint32_t bits = (uint32_t)(n < 4 ? r->xmm[n].lo : r->stack[n + 1]);

    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

DK2ML_INLINE void DK2ML_SetArgFloat(DK2ML_Regs* r, int n, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));

    if (n < 4) {
        r->xmm[n].lo = (r->xmm[n].lo & 0xFFFFFFFF00000000ull) | bits;
    } else {
        r->stack[n + 1] = bits;
    }
}

DK2ML_INLINE double DK2ML_ArgDouble(const DK2ML_Regs* r, int n)
{
    uint64_t bits = n < 4 ? r->xmm[n].lo : r->stack[n + 1];

    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

/* The result: read it in a post callback, and set it in a post or in a pre that returns DK2ML_SKIP_ORIGINAL. Integers
 * and pointers are in rax, floats and doubles in xmm0. */
DK2ML_INLINE void DK2ML_SetResult(DK2ML_Regs* r, uint64_t value)
{
    r->rax = value;
}

DK2ML_INLINE float DK2ML_ResultFloat(const DK2ML_Regs* r)
{
    uint32_t bits = (uint32_t)r->xmm[0].lo;

    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

DK2ML_INLINE void DK2ML_SetResultFloat(DK2ML_Regs* r, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));

    r->xmm[0].lo = (r->xmm[0].lo & 0xFFFFFFFF00000000ull) | bits;
}

/* --- options ---
 * Settings declared with AddOption appear under the mod on the loader's "Native mods" screen (main menu), drawn with
 * the game's own widgets. The plugin owns the values. The loader reads *value when the screen opens and writes it when
 * the player changes it, then calls onChange, which is the place to save. Callbacks run under crash containment.
 * AddOption returns DK2ML_ERROR (and logs why) for a structSize below this header's sizeof(DK2ML_Option), an unknown
 * type, a missing value pointer, FLOAT/INT with max not greater than min, or CHOICE without 1-64 choices. */

typedef enum DK2ML_OptionType {
    DK2ML_OPTION_HEADER = 0, /* section title; no value */
    DK2ML_OPTION_BOOL = 1,   /* value: bool* (1 byte); checkbox */
    DK2ML_OPTION_FLOAT = 2,  /* value: float*; slider from min to max */
    DK2ML_OPTION_INT = 3,    /* value: int*; integer slider from min to max */
    DK2ML_OPTION_CHOICE = 4, /* value: int*, index into choices; left/right selector */
    DK2ML_OPTION_KEY = 5,    /* value: int*, Windows virtual-key code. The player clicks, then presses a key or the
                              * middle, back or forward mouse button; Esc cancels. Left/right Shift, Ctrl and Alt are
                              * stored as VK_SHIFT, VK_CONTROL and VK_MENU. */
    DK2ML_OPTION_BUTTON = 6, /* no value; onChange runs on click (e.g. "Reset to defaults") */
} DK2ML_OptionType;

typedef struct DK2ML_Option {
    uint32_t structSize;        /* sizeof(DK2ML_Option); a bigger size is accepted and the extra fields ignored */
    DK2ML_OptionType type;
    const char* label;          /* UTF-8; copied, up to 512 bytes */
    const char* tooltip;        /* optional; copied, up to 512 bytes */
    void* value;                /* see the type; must stay valid for the life of the game */
    float min, max;             /* FLOAT and INT; max must be greater than min */
    const char* format;         /* FLOAT and INT, optional: one printf conversion (%f/%g/%e for FLOAT, %d/%i for INT)
                                 * plus plain text, e.g. "%.0f deg", under 64 characters. Without one, or with any
                                 * other format, the value shows as "%.2f" or "%d". */
    const char* const* choices; /* CHOICE: choiceCount UTF-8 strings (1-64); copied, up to 512 bytes each */
    int choiceCount;
    /* After the player changed the value (BUTTON: clicked). option is the loader's copy. */
    void (*onChange)(const struct DK2ML_Option* option, void* user);
    void* user;
} DK2ML_Option;

/* --- events ---
 * The loader hooks these game moments once and calls every subscriber. Subscribe from DK2ML_PluginInit. Callbacks run
 * on the main thread in subscription order, under crash containment. A crash switches the plugin off. If this game
 * build lacks the names an event needs, the loader logs it and the event never arrives. */

typedef enum DK2ML_EventType {
    DK2ML_EVENT_FRAME = 1,          /* every frame, inside ImGui's frame; not while a random map generates */
    DK2ML_EVENT_GUI_LOADED = 2,     /* the game (re)loaded its GUI; GUI items found before are gone */
    DK2ML_EVENT_STATE_CHANGED = 3, /* GameClient::m_state changed (checked once per frame). oldState and newState are
                                     * GameClient::eCGameState values (GetEnumValue, e.g. "CGAMESTATE_RUNNING");
                                     * -1 = no GameClient */
    DK2ML_EVENT_MAP_LOADED = 4, /* a mission starts or restarts (the game resets the view camera for the map) */
    DK2ML_EVENT_PLUGINS_LOADED = 5, /* once, after every plugin's DK2ML_PluginInit, before any game code;
                                     * GetInterface works from here on */
    DK2ML_EVENT_GUI_EVENT = 6, /* a game GUI event (GUI::Events::eEventType, e.g. GUI_GAME_CUSTOMIZE_OPENED);
                                     * subscribe with SubscribeGuiEvent. It arrives after the game handled it, so it
                                     * can't block it. To block one, safe-hook the game's handler. */
    DK2ML_EVENT_WINDOW_RESIZED = 7, /* the game window changed size; width/height are the new client size */
} DK2ML_EventType;

typedef struct DK2ML_Event {
    uint32_t structSize; /* sizeof(DK2ML_Event) */
    DK2ML_EventType type;
    void* gameClient; /* the GameClient (*g_pGameClient), or NULL */
    int64_t oldState, newState; /* STATE_CHANGED; for other events, newState is the current state */
    uint32_t guiEventId; /* GUI_EVENT: the event id */
    const void* guiEventParams; /* GUI_EVENT: the game's GUI::sEventParams*, may be NULL */
    int32_t width, height; /* WINDOW_RESIZED: client size in pixels */
} DK2ML_Event;

typedef void (*DK2ML_EventFn)(const DK2ML_Event* event, void* user);

/* --- interfaces ---
 * A plugin publishes a table of function pointers under a name (e.g. "author.mod.Thing"), and other plugins look it up
 * by that name. The table's first field is its structSize, and new fields go at the end. Publish from
 * DK2ML_PluginInit. Look up from PLUGINS_LOADED on, because GetInterface returns NULL during init. Look the table up
 * each time it's needed, because GetInterface returns NULL once the publisher is switched off. A crash inside another
 * plugin's function counts as a crash of the calling plugin's callback. */

/* --- tasks ---
 * AddTask runs fn(user) on the main thread at the start of the next frame, under crash containment. Use it to call
 * game functions from other threads. Tasks run in the order added. A task added while tasks run waits for the next
 * frame. Tasks don't run while a random map generates. If this game build has no frame tick, tasks queued during init
 * or PLUGINS_LOADED are dropped, and later AddTask calls fail. */
typedef void (*DK2ML_TaskFn)(void* user);

/* --- GUI kit ---
 * The game's GUI items (GUI::Item and subclasses) as pointers, operated through the game's own functions and actions.
 * Main thread only. Every function accepts NULL items. Items found before a GUI_LOADED are gone after it, so find them
 * again. If this game build lacks a name the kit needs, the loader logs it once and every Gui function does nothing.
 * GuiFind and GuiParent then return NULL, GuiName returns "", GuiChildren and GuiIsShown return 0, and the functions
 * returning DK2ML_Status return DK2ML_ERROR.
 *
 * Item events reach the plugin through its GUI XML. Add <Action type="Callback" target="<any existing item>"/> under
 * the event (OnClick, OnHover, OnScrollDown, ...) and bind it with GuiSetCallback. A clone copies its template's
 * callbacks, so set them before cloning. Callbacks run under crash containment. */

/* item: the item whose event it is (for a clone, the clone); cursorX/Y: cursor in GUI coordinates (2560x1440 canvas,
 * (0,0) at the center, +y up) */
typedef void (*DK2ML_GuiCallbackFn)(void* item, float cursorX, float cursorY, void* user);

/* Threading: AddOption, Subscribe, SubscribeGuiEvent and PublishInterface work only during DK2ML_PluginInit. Create
 * hooks there too; CreateSafeHook also works later, from any thread. EnableHook/DisableHook, Log, the PDB lookups
 * (ResolveSymbol, GetFieldOffset, GetTypeSize, GetEnumValue), GetInterface and AddTask work from any thread at any
 * time. The GUI kit is main thread only. */
typedef struct DK2ML_API {
    uint32_t apiVersion; /* the loader's DK2ML_API_VERSION */
    uint32_t structSize; /* sizeof(DK2ML_API) of the loader */

    /* Address of a game function or global by undecorated name, e.g. "GameClient::UpdateCamera". For a global it is
     * the variable's address, not its value. An overloaded name resolves to the first match and logs a warning; pass
     * the decorated name (starting with '?') to pick one. NULL if not found. */
    void* (*ResolveSymbol)(const char* name);

    /* Byte offset of a data member, e.g. GetFieldOffset("Camera", "pitch"). -1 if not found. */
    int32_t (*GetFieldOffset)(const char* typeName, const char* fieldName);

    /* sizeof a game type, e.g. GetTypeSize("Camera"). 0 if not found. */
    uint32_t (*GetTypeSize)(const char* typeName);

    /* Value of an enumerator by its C++ name, e.g. GetEnumValue("GUI::eAction", "ACTION_ADD_CHILD", &v).
     * DK2ML_OK if found. */
    DK2ML_Status (*GetEnumValue)(const char* enumType, const char* name, int64_t* out);

    /* Safe hook on target (see "safe hooks"). pre runs before the original and may change arguments or skip it. post
     * runs after it returns. Either may be NULL. The new callbacks start disabled. DK2ML_ERROR (logged) if the hook
     * can't be created or the plugin was switched off. */
    DK2ML_Status (*CreateSafeHook)(void* target, DK2ML_PreFn pre, DK2ML_PostFn post, void* user);

    /* Switch the calling plugin's safe-hook callbacks on target on or off. DK2ML_ERROR if target has no safe hook
     * (from any plugin) or the switch fails. EnableHook also fails once the plugin was switched off. */
    DK2ML_Status (*EnableHook)(void* target);
    DK2ML_Status (*DisableHook)(void* target);

    /* printf-style line to dk2ml.log, prefixed with the plugin's name. */
    void (*Log)(const char* fmt, ...);

    /* Game install folder, with trailing backslash. */
    const wchar_t* (*GetGameDir)(void);

    /* Folder for settings and other files that must survive mod updates, with trailing backslash:
     * %LOCALAPPDATA%\KillHouseGames\DoorKickers2\dk2ml\<dll name without .dll>\ (created on first call). Don't write
     * to the mod folder, because Steam replaces a Workshop mod's folder on every update. Empty string if unavailable. */
    const wchar_t* (*GetConfigDir)(void);

    /* Non-zero when the game window has focus (check before reading keys with GetAsyncKeyState). */
    int (*IsGameFocused)(void);

    /* GameClient::m_state now (GameClient::eCGameState), or -1 while there is no GameClient. */
    int64_t (*GetGameState)(void);

    /* The game's version as written into its saves and mod.xml files (e.g. 112), 0 if unknown. */
    uint32_t (*GetGameVersion)(void);

    /* The game's main window (an HWND), or NULL before it exists. */
    void* (*GetGameWindow)(void);

    /* Adds an option to the mod's page on the "Native mods" screen, in call order (see "options"). Only from
     * DK2ML_PluginInit; later calls return DK2ML_ERROR. */
    DK2ML_Status (*AddOption)(const DK2ML_Option* option);

    /* Calls fn(event, user) for every event of this type (see "events"). Only from DK2ML_PluginInit. DK2ML_ERROR for
     * an unknown type, DK2ML_EVENT_GUI_EVENT (use SubscribeGuiEvent), a NULL fn, or a later call. Subscribing twice
     * gives two callbacks. */
    DK2ML_Status (*Subscribe)(DK2ML_EventType type, DK2ML_EventFn fn, void* user);

    /* Calls fn for the game GUI event guiEventId as DK2ML_EVENT_GUI_EVENT. guiEventId is a GUI::Events::eEventType
     * value, e.g. from GetEnumValue("GUI::Events::eEventType", "GUI_GAME_CUSTOMIZE_OPENED", &v). Only from
     * DK2ML_PluginInit. DK2ML_ERROR for id 0, an id above 0xFFFF, a NULL fn, or a later call. An id past this game
     * build's last event is accepted, logged and never delivered. */
    DK2ML_Status (*SubscribeGuiEvent)(uint32_t guiEventId, DK2ML_EventFn fn, void* user);

    /* Publishes table under name with a version to raise when fields are added (see "interfaces"). name is 1-63
     * printable ASCII characters without spaces, and unique. Only from DK2ML_PluginInit. DK2ML_ERROR if the name is
     * taken or invalid, or table is NULL. table must stay valid for the life of the game. */
    DK2ML_Status (*PublishInterface)(const char* name, uint32_t version, const void* table);

    /* The table published under name, if its version is at least minVersion and the publisher is running; else NULL.
     * NULL during DK2ML_PluginInit. versionOut (optional) receives the published version. Any thread. */
    const void* (*GetInterface)(const char* name, uint32_t minVersion, uint32_t* versionOut);

    /* Queues fn(user) for the start of the next frame (see "tasks"). Any thread. DK2ML_ERROR if fn is NULL, 4096
     * tasks from all plugins are waiting, the plugin was switched off, or this game build has no frame tick. */
    DK2ML_Status (*AddTask)(DK2ML_TaskFn fn, void* user);

    /* GUI kit (see above). */
    void* (*GuiFind)(void* under, const char* name); /* first item named name under `under`, recursive; NULL = the
                                                       * whole GUI */
    void* (*GuiParent)(void* item);
    const char* (*GuiName)(void* item); /* XML name; "" if unnamed (clones have no name) */

    /* Up to max of item's children into out, in drawing order (the last is on top). Returns the child count, which can
     * exceed max. out may be NULL to only count. */
    int (*GuiChildren)(void* item, void** out, int max);

    int (*GuiIsShown)(void* item); /* the item's own flag; a shown item in a hidden parent isn't drawn */
    void (*GuiShow)(void* item, int show);

    /* Text of a StaticText, or all three state texts of a Button. Only those exact types work: subclasses and other
     * items return DK2ML_ERROR, as does a NULL utf8. A leading '@' (a game text key) is removed. The game keeps at most
     * as many characters as the item's XML text had. */
    DK2ML_Status (*GuiSetText)(void* item, const char* utf8);

    /* The game's AddChild action: moves item to the end of parent's children (drawn and clicked on top). It doesn't
     * show the item. A mod's own GUI items start at the top level. */
    DK2ML_Status (*GuiAddChild)(void* parent, void* item);

    /* The game's SetOrigin action: origin relative to the parent's center, +y up. It sets both the origin and the
     * align offset, so for an aligned item (align="l" etc.) x/y replace the XML origin. */
    DK2ML_Status (*GuiSetOrigin)(void* item, float x, float y);

    DK2ML_Status (*GuiClick)(void* item); /* runs the item's OnClick actions */

    /* Binds item's <Action type="Callback"> actions for itemEvent to fn. itemEvent is a GUI::Item::eItemEventType
     * value, e.g. from GetEnumValue("GUI::Item::eItemEventType", "EVENT_CLICK", &v). DK2ML_ERROR if fn is NULL,
     * itemEvent is out of range, the item has no Callback action for that event, or the plugin was switched off. It
     * also fails once 4096 distinct callbacks (plugin, fn, user) exist across all plugins. Binding the same fn and user
     * again reuses the existing one. */
    DK2ML_Status (*GuiSetCallback)(void* item, int itemEvent, DK2ML_GuiCallbackFn fn, void* user);

    /* While any plugin captures (1), the game acts as if one of its menus is open, so clicks and keys don't reach the
     * map. Release with 0. A switched-off plugin's capture is released, and its capture calls return DK2ML_ERROR.
     * If this game build lacks GameGUI::IsAnyMenuOpened, the call returns DK2ML_OK and has no effect. */
    DK2ML_Status (*CaptureGameInput)(int capture);

    /* Non-zero if one of the game's own menus is open, ignoring captures. */
    int (*IsGameMenuOpen)(void);
} DK2ML_API;

typedef struct DK2ML_PluginInfo {
    const wchar_t* pluginPath; /* full path of the plugin DLL */
    const wchar_t* pluginDir; /* folder of the plugin DLL, with trailing backslash */
    const wchar_t* modDir; /* mod folder, with trailing backslash; empty for mods_native */
} DK2ML_PluginInfo;

typedef int (*DK2ML_PluginInitFn)(const DK2ML_API* api, const DK2ML_PluginInfo* info);

#define DK2ML_PLUGIN_INIT_NAME "DK2ML_PluginInit"

/* --- manifest ---
 * Optional. Shown on the "Native mods" screen, in dk2ml.log and in crash reports. The loader reads it from the DLL file
 * without running plugin code. A loader whose DK2ML_API_VERSION is below minApiVersion doesn't load the plugin and
 * says why. Texts are UTF-8, cut to their array size. Use once, at file scope:
 *   DK2ML_PLUGIN_MANIFEST(DK2ML_API_VERSION, "Free Camera", "1.2.0", "Some Author", "https://...", 112);
 * Arguments: minApiVersion, name, version, author, url (may be ""), gameVersion (may be 0). */
typedef struct DK2ML_Manifest {
    uint32_t structSize; /* sizeof(DK2ML_Manifest) */
    uint32_t minApiVersion; /* lowest DK2ML_API_VERSION the plugin needs */
    char name[64]; /* player-facing name */
    char version[32]; /* plugin version, e.g. "1.2.0" */
    char author[64];
    char url[160]; /* homepage or Workshop page; shown as text */
    uint32_t gameVersion; /* game version it was made for, as in mod.xml (e.g. 112); shown, not enforced */
} DK2ML_Manifest;

#define DK2ML_MANIFEST_NAME "DK2ML_PluginManifest"
#define DK2ML_PLUGIN_MANIFEST(minApiVersion, name, version, author, url, gameVersion)                                  \
    DK2ML_EXPORT const DK2ML_Manifest DK2ML_PluginManifest = {                                                         \
        sizeof(DK2ML_Manifest), (minApiVersion), name, version, author, url, (gameVersion)}

#ifdef __cplusplus
}
#define DK2ML_EXPORT extern "C" __declspec(dllexport)
#else
#define DK2ML_EXPORT __declspec(dllexport)
#endif
