#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>
#include <vector>

#include "dk2ml.h"
#include "dk2ml_version.h" // DK2ML_VERSION, generated from CMakeLists.txt

// --- RealDbgHelp.cpp ---
// System32's dbghelp.dll, which every export of the dbghelp.dll stub forwards to, and which Symbols.cpp uses
bool RealDbghelp_Load(); // DllMain (stub and loader), process attach
HMODULE RealDbghelp();

// --- Log.cpp ---
void LogOpen(const std::wstring& path);
void LogLine(const char* prefix, const char* fmt, va_list args);
void LogF(const char* fmt, ...);
bool LogTryF(const char* fmt, ...); // LogF unless the log is busy (crash path)
void LogEchoToStdout(bool on); // tools: also print every line
std::wstring Log_ModuleName(HMODULE module); // a module's file name for log lines ("plugin" if unknown)

// --- Symbols.cpp ---
bool Symbols_Init(const std::wstring& gameDir, HMODULE exe);
void* Symbols_Resolve(const char* name);
int32_t Symbols_FieldOffset(const char* typeName, const char* fieldName);
uint32_t Symbols_TypeSize(const char* typeName);
bool Symbols_EnumValue(const char* enumType, const char* name, int64_t* out);

// The exe is linked with /OPT:ICF, which folds identical functions into one, so a hook on one name runs for all of
// them. Logs a warning naming the others and returns how many functions share the address (1 = only one).
int Symbols_WarnIfShared(void* address, const char* who);
std::string Symbols_NameAt(void* address); // a function's name, "" if unknown
std::vector<std::string> Symbols_NamesAt(void* address); // every name at the address (more than one: folded code)
// "Function+0x12" in the game, without waiting or allocating (the crash report)
bool Symbols_DescribeTry(uintptr_t address, char* out, size_t size);

// symtest --check-index: compares the name index with dbghelp's per-call answers for `names` (undecorated), plus
// `samples` each of folded, single, overloaded and unique entries. Returns the mismatch count, -1 without an index.
int Symbols_CheckIndex(const std::vector<std::string>& names, size_t samples);

// Explorer (symtest --find/--types/--type/--enum). Masks use '*' and '?', case-insensitive.
struct FoundSymbol {
    std::string name, decorated; // decorated: the public name, which picks this overload in ResolveSymbol ("" if none)
    uint64_t address;

    enum Kind { Function, Global, Public } kind;

    int foldedWith; // names at this address (more than 1: identical code folding)
};

std::vector<FoundSymbol> Symbols_Find(const char* mask, size_t limit); // more than limit results: stopped there
std::vector<std::string> Symbols_FindTypes(const char* mask); // struct/class/union names, "enum X" for enums

struct TypeMember {
    std::string name, type;
    int32_t offset;
    uint64_t size;

    bool base; // a base class (name "(base class)", type its name)
    bool bitfield; // offset is the storage unit's
    uint32_t bitPosition, bitLength;
};

// the layout plugins get from GetFieldOffset (the copy that fits this build); otherCopies: other layouts in the PDB
bool Symbols_DescribeType(const char* typeName, std::vector<TypeMember>* members, uint64_t* size, int* otherCopies);
bool Symbols_EnumList(const char* enumType, std::vector<std::pair<std::string, int64_t>>* values);

// --- SafeHook.cpp + SafeHookEntry.asm ---
// DK2ML_API::CreateSafeHook (needs MH_Initialize first). owner = the plugin. Several owners may hook one target (a
// chain). After a callback crashes, every callback of its owner passes through.
using SafeHookFaultFn = void (*)(HMODULE owner, DWORD code, void* address);

// starts disabled
DK2ML_Status SafeHook_Create(void* target, DK2ML_PreFn pre, DK2ML_PostFn post, void* user, HMODULE owner);
// owner's callbacks on target; 0 = not a safe hook, -1 = error
int SafeHook_SetEnabled(void* target, HMODULE owner, bool enabled);
void SafeHook_Remove(void* target); // the whole chain (tests)
void SafeHook_FaultOwner(HMODULE owner); // make all of owner's safe hooks pass through
void SafeHook_SetFaultCallback(SafeHookFaultFn fn); // called after a contained crash
void SafeHook_SetTargetNamer(std::string (*namer)(void* target)); // names hooked functions in log lines (optional)

struct SafeHookChain {
    void* target;
    std::vector<HMODULE> owners; // in the order their pre callbacks run
};

std::vector<SafeHookChain> SafeHook_Chains(); // every safe-hooked target

// For the crash report's stack walk. A post-hooked call returns to SafeHookPostEntry, and its real return address is
// on this thread's side stack.
struct SafeHookPostFrame {
    uintptr_t returnAddress; // where the hooked call really returns
    void* target; // the hooked function
};

uintptr_t SafeHook_PostEntryAddress();
int SafeHook_PostFrames(SafeHookPostFrame* out, int max); // this thread's, innermost first
int SafeHook_OwnersOfTry(void* target, HMODULE* out, int max); // -1 if the lock is busy (never waits)

// --- Consent.cpp ---
// Which mod folders may run code, and the player's permission for Workshop items (per version of their native folder)
enum class ModSource { Local, Workshop, Unknown };

struct NativeFile {
    std::wstring rel, full; // path relative to the native folder, full path
    uint64_t size;
};

// A Workshop item to ask about: one startup prompt lists every item that's new or changed since the last answer
struct ConsentRequest {
    std::wstring itemId, title;
    std::wstring nativeDir; // the item's native\ folder, with trailing backslash
    std::wstring fingerprint; // set by Consent_DecideAll (empty: unreadable)
    bool changed = false; // set by Consent_DecideAll: answered before, for different code
};

// A plugin's DK2ML_PluginManifest, read from its file without running it. Texts are cut to their arrays and control
// characters become spaces. It's the plugin's own claim and is shown as such.
struct PluginManifest {
    bool present = false;
    uint32_t structSize = 0, minApiVersion = 0, gameVersion = 0;
    std::string name, version, author, url; // UTF-8
};

using ConsentPromptFn = bool (*)(const std::vector<ConsentRequest>& pending); // true = allow them all

// final path (links resolved), trailing backslash; empty on failure
std::wstring Consent_Canonical(const std::wstring& path);
// Steam keeps a game's Workshop items in the game's own library: <library>\steamapps\workshop\content\1239080\.
// From the canonical game folder; empty if it isn't in a Steam library.
std::wstring Consent_WorkshopRoot(const std::wstring& canonicalGameDir);
ModSource Consent_Classify(const std::wstring& canonicalDir, const std::vector<std::wstring>& localRoots,
                           const std::wstring& workshopRoot, std::wstring* workshopId);

bool Consent_IsPlugin(const std::wstring& dllPath); // exports DK2ML_PluginInit (checked without running it)
bool Consent_ReadPlugin(const std::wstring& dllPath, PluginManifest* manifest); // the same, plus its manifest if any
std::string Consent_ManifestLine(const PluginManifest& m); // "\"Name\" 1.0 by Author", "" without a manifest

// recursive; false if unreadable/too big/links
bool Consent_ListFiles(const std::wstring& dir, std::vector<NativeFile>* files);
// sha256 hex of every file's path + content; empty on failure
std::wstring Consent_Fingerprint(const std::wstring& dir);
std::wstring Consent_ModTitle(const std::wstring& modDir); // mod.xml title, entities decoded
std::string Consent_XmlDecode(const std::string& s); // XML attribute text: named and numeric entities, one pass
std::wstring Consent_FileHash(const std::wstring& path); // sha256 hex of one file; empty on failure

// A dependency DLL a mod ships next to its plugins. Two mods with different files under one name clash: Windows
// loads the name once, so one of the plugins gets the other's copy.
struct SupportDll {
    size_t mod; // whose (an index the caller chooses)
    std::wstring name, hash; // file name, Consent_FileHash
};

struct DllClash {
    std::wstring name;
    size_t modA, modB;
};

// same name (any case), other content
std::vector<DllClash> Consent_FindDllClashes(const std::vector<SupportDll>& dlls);

// The Mods menu's changes during a session. The game's active list and options.xml hold the same strings, so mod
// folders compare by key (backslashes, trailing backslash, lowercase) without touching the file system.
std::wstring Consent_ModListKey(const std::wstring& path); // "" for ""

struct EnabledDiff {
    std::vector<bool> active; // per known folder: still in the list
    std::vector<std::wstring> added; // listed folders not known yet, once each, in list order (slashes fixed)
};

EnabledDiff Consent_DiffEnabled(const std::vector<std::wstring>& known, const std::vector<std::wstring>& now);

bool Consent_Prompt(const std::vector<ConsentRequest>& pending); // the real dialog
std::wstring Consent_PromptText(const std::vector<ConsentRequest>& pending); // its text (consenttest checks it)
// Fingerprints every item and checks dk2ml.ini [workshop] (<id>=allow:<fp> or deny:<fp>). One prompt lists the items
// without an answer for their current code, and the answer is saved for each. load: per item, whether it may run.
void Consent_DecideAll(const std::wstring& ini, std::vector<ConsentRequest>& items, ConsentPromptFn prompt,
                       std::vector<bool>* load);
void Consent_EnsureIni(const std::wstring& ini); // writes the default dk2ml.ini if there is none

// --- OptionCheck.cpp ---
// AddOption's rules, also used by symtest's dry run
constexpr size_t kMaxOptionText = 512; // option texts are copied up to this many bytes
// nullptr if the option is fine, else why it's refused. structSize must be at least sizeof(DK2ML_Option); extra bytes
// are ignored. copy: the loader's copy of the option.
const char* Options_Check(const DK2ML_Option* option, DK2ML_Option* copy);
// the format to show a FLOAT/INT value with: the plugin's, or a default if it isn't one plain conversion
std::string Options_SafeFormat(const std::string& format, bool integer);

// --- Plugins.cpp ---
enum class ModStatus {
    Loaded, // all its DLLs initialized
    Declined, // Workshop item, the player said no (for this version)
    PathNotAllowed, // neither a local mod folder nor a Door Kickers 2 Workshop item
    InitFailed, // DK2ML_PluginInit returned non-zero
    Crashed, // crashed (in init, a hook, an option, event or GUI callback, or a task) and switched off
    LoadFailed, // LoadLibrary failed or no DK2ML_PluginInit
    Unreadable, // its files couldn't be read/verified
    Duplicate, // a plugin with the same file name is already loaded (from another mod)
    NeedsNewerLoader, // its manifest asks for a newer API than this loader's
    TurnedOff, // was running, then disabled in the Mods menu: switched off (its code stays until a restart)
    EnabledLater, // enabled in the Mods menu after the game started: loads at the next start
};

struct ModEntry {
    std::wstring title, modDir, workshopId;
    ModSource source;
    bool gameNativeFolder; // <game>\mods_native, not a mod

    std::vector<std::wstring> dllNames; // plugins (export DK2ML_PluginInit)
    std::vector<PluginManifest> manifests; // one per dllNames entry (present = false: it has none)
    uint32_t neededApi = 0; // NeedsNewerLoader: the plugin API version its manifest asks for
    std::vector<std::wstring> supportNames; // other DLLs in native\ (dependencies), not loaded as plugins
    std::vector<HMODULE> modules; // loaded plugins

    std::wstring duplicateOf; // Duplicate: the mod whose copy loaded
    std::vector<std::string> missing; // names its plugins looked up in init that this game build doesn't have
    std::vector<std::string> conflicts; // dependency DLL names shared with another mod or the game, for its page
    std::wstring codeDir; // where its plugins load from (its native folder)
    ModStatus status;

    std::wstring listKey; // Consent_ModListKey of the folder as the game lists it ("" for mods_native)
    bool enabledNow = true; // in the game's active list as of the last GUI load
    bool seenInGame = false; // has been in the game's list (only then does leaving it switch the mod off)
};

// An option a plugin declared with DK2ML_API::AddOption: the loader's own copy of its texts.
struct OptionEntry {
    HMODULE owner;
    std::string label, tooltip, format;
    std::vector<std::string> choices;
    std::vector<const char*> choicePointers;
    DK2ML_Option api; // what onChange gets: points into the strings above
    bool faulted; // its plugin was switched off: no more calls
};

void Plugins_LoadAll(const std::wstring& gameDir);
// Each GUI load (main thread): applies the Mods menu's changes since startup. gameList: the game's active mod folders
// (null: re-read options.xml). A running mod no longer listed is switched off. A newly listed one with plugins is
// recorded to load at the next start.
void Plugins_SyncEnabled(const std::vector<std::string>* gameList);
bool Plugins_ModListFromFile(); // the last sync re-read options.xml (the game's list isn't available in this build)
bool Plugins_RestartNeeded(); // a change that only a restart applies (mods turned on or off)
const DK2ML_API* Plugins_Api();
std::vector<ModEntry>& Plugins_Mods(); // main thread only
std::vector<OptionEntry*>& Plugins_Options(); // in the order added
void Plugins_OnOptionCrash(HMODULE owner);
const std::wstring& Plugins_Ini(); // dk2ml.ini
std::wstring Plugins_SaveDir(); // %LOCALAPPDATA%\KillHouseGames\DoorKickers2\ (empty if unknown)
void Plugins_LogSharedHooks(); // after everything is hooked: functions with hooks from more than one module

// for the crash report (CrashHelpers), alloc-free: the mod whose code an address is in; the native mods section
bool Plugins_CrashOwner(uintptr_t address, char* out, size_t size);
struct CrashText;
void Plugins_CrashMods(CrashText* out);

// --- Events.cpp ---
// DK2ML_API::Subscribe; GameHooks.cpp feeds it from the loader's hooks. Doesn't touch the game, so eventstest runs it
// without the game.
void Events_SetOpen(bool open); // subscriptions are taken only while plugins initialize
void Events_SetCrashCallback(void (*onCrash)(HMODULE owner)); // a subscriber crashed: switch its plugin off
const char* Events_Check(int type, DK2ML_EventFn fn); // nullptr if a subscription is valid, else why not
DK2ML_Status Events_Subscribe(HMODULE owner, int type, DK2ML_EventFn fn, void* user);
bool Events_Wanted(DK2ML_EventType type); // anyone subscribed
void Events_FaultOwner(HMODULE owner); // no more events to it
void Events_Dispatch(DK2ML_EventType type, void* gameClient, int64_t oldState, int64_t newState);
void Events_Frame(void* gameClient, int64_t state); // each frame: STATE_CHANGED if the state changed, then FRAME
void Events_LogSubscribers();
std::vector<std::string> Events_TypesOf(HMODULE owner); // the events a plugin subscribed to ("FRAME", ...)

// GUI events (SubscribeGuiEvent): by the game's GUI event id; the loader's event consumer (GuiKit.cpp) delivers them
const char* Events_CheckGui(uint32_t id, DK2ML_EventFn fn); // nullptr if valid, else why not
DK2ML_Status Events_SubscribeGui(HMODULE owner, uint32_t id, DK2ML_EventFn fn, void* user);
std::vector<uint32_t> Events_GuiEventIds(); // every id someone subscribed to, sorted
void Events_DispatchGui(uint32_t id, const void* params, void* gameClient, int64_t state);
void Events_DispatchResize(void* gameClient, int64_t state, int32_t width, int32_t height);

// tasks (DK2ML_API::AddTask): any thread queues, the frame tick runs them first thing (Events_Frame)
DK2ML_Status Events_AddTask(HMODULE owner, DK2ML_TaskFn fn, void* user);
void Events_RunTasks();
void Events_SetTasksAvailable(bool available); // false: no frame tick in this game build, AddTask is refused

// --- Interfaces.cpp ---
// DK2ML_API::PublishInterface/GetInterface. Published only while plugins initialize, looked up only after. Doesn't
// touch the game, so eventstest runs it without the game.
constexpr size_t kMaxInterfaceName = 63;
void Interfaces_SetOpen(bool open);
const char* Interfaces_Check(const char* name, const void* table); // nullptr if publishing is valid, else why not
DK2ML_Status Interfaces_Publish(HMODULE owner, const char* name, uint32_t version, const void* table);
const void* Interfaces_Get(HMODULE caller, const char* name, uint32_t minVersion, uint32_t* versionOut);
void Interfaces_FaultOwner(HMODULE owner); // GetInterface stops returning its tables
void Interfaces_Of(HMODULE owner, std::vector<std::string>* published, std::vector<std::string>* used);
void Interfaces_Log();

// --- CrashReport.cpp ---
// A report for an unhandled exception, <game>\dk2ml-crash-<time>.log, written from the game's own crash handler
// (CreateMiniDump) just before its dump. Runs in a dying process: no heap, never waits on a lock.
struct CrashText { // a fixed buffer, cut off when full
    char* buf;
    size_t cap, len;
    void Add(const char* fmt, ...);
};

struct CrashFrame {
    uintptr_t pc;
    uintptr_t functionStart; // from the unwind data (0: none, e.g. a leaf or generated code)
    void* returnedThroughPost; // this frame was reached through a post hook on that function
};

struct CrashHelpers { // the loader's knowledge, alloc-free; crashtest fakes them
    bool (*describe)(uintptr_t address, char* out, size_t size); // "Function+0x12" in game code, false if unknown
    bool (*ownerOf)(uintptr_t address, char* out, size_t size); // "x.dll (mod "Title", ...)" for a mod's code
    void (*mods)(CrashText* out); // the native mods section
};

int Crash_Walk(const CONTEXT& context, CrashFrame* frames, int max); // innermost first, through post hooks
void Crash_Format(const EXCEPTION_RECORD& record, const CONTEXT& context, bool mainThread, const CrashHelpers& helpers,
                  CrashText* out);
void CrashReport_SetHelpers(const CrashHelpers& helpers);
void CrashReport_Init(const std::wstring& dir); // main thread, at startup: where reports go; keeps the newest ones
bool CrashReport_Write(const EXCEPTION_POINTERS* pointers); // writes the report (once per process); false if not
const wchar_t* CrashReport_LastPath(); // the report written, "" if none

// --- GuiThunks.cpp ---
// Plugin GUI callbacks (thunks into crash-contained calls) and input capture. Doesn't touch the game, so guitest runs
// it without the game.
struct GuiCallbackLayout {
    int32_t actionOwner; // GUI::sAction::owner (-1: unknown)
    int32_t actionCursor; // GUI::sAction::eventParams + GUI::sEventParams::cursor (a Vector2; -1: unknown)
};

void Gui_SetCallbackLayout(const GuiCallbackLayout& layout);
void Gui_SetCrashCallback(void (*onCrash)(HMODULE owner)); // a callback crashed: switch its plugin off
void* Gui_CallbackThunk(HMODULE owner, DK2ML_GuiCallbackFn fn, void* user); // what pCallback gets; nullptr if full
bool Gui_IsThunk(const void* address);
int Gui_CallbackCount(HMODULE owner); // distinct callbacks it set
void Gui_FaultOwner(HMODULE owner); // its callbacks do nothing from now on, its capture is released
DK2ML_Status Gui_SetCapture(HMODULE owner, bool capture);
bool Gui_AnyCapture();
bool Gui_Captures(HMODULE owner);

// --- GuiKit.cpp ---
// DK2ML_API::Gui* over the game's GUI, the game-event consumer, the game window and version
void GuiKit_Init(); // after Symbols_Init (GameHooks_Init): resolves the kit's and services' names
void GuiKit_OnGuiLoaded(bool screen); // each GUIManager::Load: (re)registers the event consumer (screen: event 219 too)
bool GuiKit_GuiEventsAvailable();
uint32_t GuiKit_GameVersion(); // 0 if unknown; any thread, any time
void* GuiKit_GameWindow(); // HWND, nullptr before the window exists

void* Gui_Find(void* under, const char* name);
void* Gui_Parent(void* item);
const char* Gui_Name(void* item);
int Gui_Children(void* item, void** out, int max);
int Gui_IsShown(void* item);
void Gui_Show(void* item, int show);
DK2ML_Status Gui_SetText(void* item, const char* utf8);
DK2ML_Status Gui_AddChild(void* parent, void* item);
DK2ML_Status Gui_SetOrigin(void* item, float x, float y);
DK2ML_Status Gui_Click(void* item);
DK2ML_Status Gui_SetCallback(HMODULE owner, void* item, int itemEvent, DK2ML_GuiCallbackFn fn, void* user);

// --- GameHooks.cpp ---
// The loader's own game hooks: the frame tick and GUI loading (events, the menu), the map-load signal, input capture,
// window resizes. Owned by the loader's module.
void GameHooks_Init();
bool GameHooks_Install(void* target, DK2ML_PreFn pre, DK2ML_PostFn post, const char* what); // logs a failure
int64_t GameHooks_GameState(void** gameClient); // GameClient::m_state now (-1: none or unknown), and the GameClient
int GameHooks_IsGameMenuOpen(); // GameGUI::IsAnyMenuOpened without the plugins' captures (0 if unknown)

// --- gui/nativemods/NativeModsButton.cpp ---
// The "Native mods" button and screen on the main menu, merged into the GUI while it loads
bool Menu_Init(bool hooksReady); // hooksReady: the frame tick and GUI load are hooked
bool Menu_Ready();
void Menu_OnFrame();
void Menu_OnGuiLoadBegin();
void Menu_OnGuiLoadEnd();
void Menu_OnGuiLoaded();
