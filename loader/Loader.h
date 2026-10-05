#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>
#include <vector>

#include "dk2ml.h"
#include "dk2ml_version.h" // DK2ML_VERSION, generated

// --- RealDbgHelp.cpp ---
// System32's dbghelp: the stub's forwarding target, also used by Symbols.cpp
bool RealDbghelp_Load(); // both DllMains, process attach
HMODULE RealDbghelp();

// --- Log.cpp ---
void LogOpen(const std::wstring& path);
void LogLine(const char* prefix, const char* fmt, va_list args);
void LogF(const char* fmt, ...);
bool LogTryF(const char* fmt, ...); // LogF unless the log is busy (crash path)
void LogEchoToStdout(bool on); // tools
std::wstring Log_ModuleName(HMODULE module); // file name; "plugin" if unknown

// --- Symbols.cpp ---
bool Symbols_Init(const std::wstring& gameDir, HMODULE exe);
void* Symbols_Resolve(const char* name);
int32_t Symbols_FieldOffset(const char* typeName, const char* fieldName);
uint32_t Symbols_TypeSize(const char* typeName);
bool Symbols_EnumValue(const char* enumType, const char* name, int64_t* out);

// /OPT:ICF folds identical functions, so a hook on one name runs for all. Warns naming the others; returns how many
// names share the address (1: not folded).
int Symbols_WarnIfShared(void* address, const char* who);
std::string Symbols_NameAt(void* address); // "" if unknown
std::vector<std::string> Symbols_NamesAt(void* address); // several: folded code
// "Function+0x12"; never waits or allocates (crash report)
bool Symbols_DescribeTry(uintptr_t address, char* out, size_t size);

// symtest --check-index: the index vs per-call dbghelp for `names` and `samples` per kind. Mismatches; -1: no index.
int Symbols_CheckIndex(const std::vector<std::string>& names, size_t samples);

// Explorer (symtest --find/--types/--type/--enum). Masks use '*' and '?', case-insensitive.
struct FoundSymbol {
    std::string name, decorated; // decorated: picks this overload in ResolveSymbol ("" if none)
    uint64_t address;

    enum Kind { Function, Global, Public } kind;

    int foldedWith; // names at this address (>1: ICF)
};

std::vector<FoundSymbol> Symbols_Find(const char* mask, size_t limit); // stops past limit
std::vector<std::string> Symbols_FindTypes(const char* mask); // struct/class/union names, "enum X" for enums

struct TypeMember {
    std::string name, type;
    int32_t offset;
    uint64_t size;

    bool base; // name "(base class)", type its name
    bool bitfield; // offset is the storage unit's
    uint32_t bitPosition, bitLength;
};

// GetFieldOffset's layout (ChooseType); otherCopies: other layouts in the PDB
bool Symbols_DescribeType(const char* typeName, std::vector<TypeMember>* members, uint64_t* size, int* otherCopies);
bool Symbols_EnumList(const char* enumType, std::vector<std::pair<std::string, int64_t>>* values);

// --- SafeHook.cpp + SafeHookEntry.asm ---
// CreateSafeHook (after MH_Initialize); see the top of SafeHook.cpp.
using SafeHookFaultFn = void (*)(HMODULE owner, DWORD code, void* address);

// starts disabled
DK2ML_Status SafeHook_Create(void* target, DK2ML_PreFn pre, DK2ML_PostFn post, void* user, HMODULE owner);
// owner's callbacks on target; 0 = not a safe hook, -1 = error
int SafeHook_SetEnabled(void* target, HMODULE owner, bool enabled);
void SafeHook_Remove(void* target); // the whole chain (tests)
void SafeHook_FaultOwner(HMODULE owner); // all its callbacks pass through
void SafeHook_SetFaultCallback(SafeHookFaultFn fn); // called after a contained crash
void SafeHook_SetTargetNamer(std::string (*namer)(void* target)); // optional, for log lines

struct SafeHookChain {
    void* target;
    std::vector<HMODULE> owners; // pre order
};

std::vector<SafeHookChain> SafeHook_Chains();

// For the crash report's stack walk: a post-hooked call returns to SafeHookPostEntry; the real address is on the side
// stack.
struct SafeHookPostFrame {
    uintptr_t returnAddress; // the real one
    void* target;
};

uintptr_t SafeHook_PostEntryAddress();
int SafeHook_PostFrames(SafeHookPostFrame* out, int max); // this thread's, innermost first
int SafeHook_OwnersOfTry(void* target, HMODULE* out, int max); // -1 if the lock is busy (never waits)

// --- Consent.cpp ---
// Which mod folders may run code; Workshop permission per version of the native folder
enum class ModSource { Local, Workshop, Unknown };

struct NativeFile {
    std::wstring rel, full; // relative to native\, full
    uint64_t size;
};

// A Workshop item for the startup prompt
struct ConsentRequest {
    std::wstring itemId, title;
    std::wstring nativeDir; // trailing backslash
    std::wstring fingerprint; // Consent_DecideAll; empty: unreadable
    bool changed = false; // Consent_DecideAll: answered before, for other code
};

// DK2ML_PluginManifest, read without running the plugin. Texts cut to their arrays, control characters -> spaces.
// The plugin's own claim.
struct PluginManifest {
    bool present = false;
    uint32_t structSize = 0, minApiVersion = 0, gameVersion = 0;
    std::string name, version, author, url; // UTF-8
};

using ConsentPromptFn = bool (*)(const std::vector<ConsentRequest>& pending); // true = allow them all

// final path (links resolved), trailing backslash; empty on failure
std::wstring Consent_Canonical(const std::wstring& path);
// <the game's Steam library>\steamapps\workshop\content\1239080\; empty outside a Steam library
std::wstring Consent_WorkshopRoot(const std::wstring& canonicalGameDir);
ModSource Consent_Classify(const std::wstring& canonicalDir, const std::vector<std::wstring>& localRoots,
                           const std::wstring& workshopRoot, std::wstring* workshopId);

bool Consent_IsPlugin(const std::wstring& dllPath); // exports DK2ML_PluginInit; nothing runs
bool Consent_ReadPlugin(const std::wstring& dllPath, PluginManifest* manifest); // plus its manifest
std::string Consent_ManifestLine(const PluginManifest& m); // "\"Name\" 1.0 by Author", "" without a manifest

// recursive; false if unreadable/too big/links
bool Consent_ListFiles(const std::wstring& dir, std::vector<NativeFile>* files);
// sha256 hex of every file's path + content; empty on failure
std::wstring Consent_Fingerprint(const std::wstring& dir);
std::wstring Consent_ModTitle(const std::wstring& modDir); // mod.xml title, entities decoded
std::string Consent_XmlDecode(const std::string& s); // named and numeric entities, one pass
std::wstring Consent_FileHash(const std::wstring& path); // sha256 hex of one file; empty on failure

// A mod's dependency DLL. Different files under one name clash: Windows loads the name once.
struct SupportDll {
    size_t mod; // caller's index
    std::wstring name, hash; // file name, Consent_FileHash
};

struct DllClash {
    std::wstring name;
    size_t modA, modB;
};

// same name (any case), other content
std::vector<DllClash> Consent_FindDllClashes(const std::vector<SupportDll>& dlls);

// Mods-menu sync: folders compare by key (backslashes, trailing backslash, lowercase), no file-system calls.
std::wstring Consent_ModListKey(const std::wstring& path); // "" for ""

struct EnabledDiff {
    std::vector<bool> active; // per known folder: still listed
    std::vector<std::wstring> added; // new folders, once each, list order, slashes fixed
};

EnabledDiff Consent_DiffEnabled(const std::vector<std::wstring>& known, const std::vector<std::wstring>& now);

bool Consent_Prompt(const std::vector<ConsentRequest>& pending); // the real dialog
std::wstring Consent_PromptText(const std::vector<ConsentRequest>& pending); // consenttest
// Checks fingerprints against dk2ml.ini [workshop] (<id>=allow:<fp> or deny:<fp>); one prompt for the unanswered
// items, answer saved for each. load: per item.
void Consent_DecideAll(const std::wstring& ini, std::vector<ConsentRequest>& items, ConsentPromptFn prompt,
                       std::vector<bool>* load);
void Consent_EnsureIni(const std::wstring& ini); // writes the default if missing

// --- OptionCheck.cpp ---
// AddOption's rules, also used by symtest's dry run
constexpr size_t kMaxOptionText = 512; // bytes copied per text
// nullptr if fine, else why refused. structSize >= sizeof(DK2ML_Option); extra bytes ignored. copy: the loader's.
const char* Options_Check(const DK2ML_Option* option, DK2ML_Option* copy);
// the plugin's FLOAT/INT format, or a default if it isn't one plain conversion
std::string Options_SafeFormat(const std::string& format, bool integer);

// --- Plugins.cpp ---
enum class ModStatus {
    Loaded,
    Declined, // Workshop permission refused (this version)
    PathNotAllowed, // not a local mod folder or a DK2 Workshop item
    InitFailed, // DK2ML_PluginInit returned non-zero
    Crashed, // contained crash, switched off
    LoadFailed, // LoadLibrary failed or no DK2ML_PluginInit
    Unreadable, // files couldn't be read/verified
    Duplicate, // same plugin file name already loaded from another mod
    NeedsNewerLoader, // manifest's minApiVersion > DK2ML_API_VERSION
    TurnedOff, // disabled in the Mods menu while running; code stays until a restart
    EnabledLater, // enabled in the Mods menu after start; loads next start
};

struct ModEntry {
    std::wstring title, modDir, workshopId;
    ModSource source;
    bool gameNativeFolder; // <game>\mods_native, not a mod

    std::vector<std::wstring> dllNames; // plugins (export DK2ML_PluginInit)
    std::vector<PluginManifest> manifests; // parallel to dllNames
    uint32_t neededApi = 0; // NeedsNewerLoader: the manifest's API version
    std::vector<std::wstring> supportNames; // dependencies in native\, not loaded
    std::vector<HMODULE> modules;

    std::wstring duplicateOf; // Duplicate: the mod whose copy loaded
    std::vector<std::string> missing; // names looked up in init that this build lacks
    std::vector<std::string> conflicts; // dependency DLL names shared with another mod or the game
    std::wstring codeDir; // its native folder
    ModStatus status;

    std::wstring listKey; // Consent_ModListKey ("" for mods_native)
    bool enabledNow = true; // in the game's list at the last GUI load
    bool seenInGame = false; // only then does leaving the list switch it off
};

// An AddOption option, with the loader's copy of its texts.
struct OptionEntry {
    HMODULE owner;
    std::string label, tooltip, format;
    std::vector<std::string> choices;
    std::vector<const char*> choicePointers;
    DK2ML_Option api; // onChange's view; points into the strings above
    bool faulted; // owner switched off: no more calls
};

void Plugins_LoadAll(const std::wstring& gameDir);
// Each GUI load (main thread): applies Mods-menu changes. gameList: the game's active folders (null: re-read
// options.xml). Unlisted running mods switch off; newly listed ones load next start.
void Plugins_SyncEnabled(const std::vector<std::string>* gameList);
bool Plugins_ModListFromFile(); // the last sync re-read options.xml (no game list in this build)
bool Plugins_RestartNeeded(); // mods turned on or off since start
const DK2ML_API* Plugins_Api();
std::vector<ModEntry>& Plugins_Mods(); // main thread only
std::vector<OptionEntry*>& Plugins_Options(); // in the order added
void Plugins_OnOptionCrash(HMODULE owner);
const std::wstring& Plugins_Ini(); // dk2ml.ini
std::wstring Plugins_SaveDir(); // %LOCALAPPDATA%\KillHouseGames\DoorKickers2\ (empty if unknown)
void Plugins_LogSharedHooks(); // after startup: functions hooked by several modules

// crash report (CrashHelpers), alloc-free: the mod owning an address; the plugins section
bool Plugins_CrashOwner(uintptr_t address, char* out, size_t size);
struct CrashText;
void Plugins_CrashMods(CrashText* out);

// --- Events.cpp ---
// Subscribe, fed by GameHooks.cpp. No game access (eventstest).
void Events_SetOpen(bool open); // only during plugin init
void Events_SetCrashCallback(void (*onCrash)(HMODULE owner)); // switches the subscriber's plugin off
const char* Events_Check(int type, DK2ML_EventFn fn); // nullptr if valid, else why not
DK2ML_Status Events_Subscribe(HMODULE owner, int type, DK2ML_EventFn fn, void* user);
bool Events_Wanted(DK2ML_EventType type); // anyone subscribed
void Events_FaultOwner(HMODULE owner); // no more events to it
void Events_Dispatch(DK2ML_EventType type, void* gameClient, int64_t oldState, int64_t newState);
void Events_Frame(void* gameClient, int64_t state); // STATE_CHANGED if changed, then FRAME
void Events_LogSubscribers();
std::vector<std::string> Events_TypesOf(HMODULE owner); // "FRAME", ...

// GUI events (SubscribeGuiEvent) by game event id, delivered by GuiKit.cpp's consumer
const char* Events_CheckGui(uint32_t id, DK2ML_EventFn fn); // nullptr if valid, else why not
DK2ML_Status Events_SubscribeGui(HMODULE owner, uint32_t id, DK2ML_EventFn fn, void* user);
std::vector<uint32_t> Events_GuiEventIds(); // sorted
void Events_DispatchGui(uint32_t id, const void* params, void* gameClient, int64_t state);
void Events_DispatchResize(void* gameClient, int64_t state, int32_t width, int32_t height);

// tasks (AddTask): queued from any thread, run first in Events_Frame
DK2ML_Status Events_AddTask(HMODULE owner, DK2ML_TaskFn fn, void* user);
void Events_RunTasks();
void Events_SetTasksAvailable(bool available); // false: no frame tick; AddTask refused

// --- Interfaces.cpp ---
// PublishInterface/GetInterface: publish during init, look up after. No game access (eventstest).
constexpr size_t kMaxInterfaceName = 63;
void Interfaces_SetOpen(bool open);
const char* Interfaces_Check(const char* name, const void* table); // nullptr if valid, else why not
DK2ML_Status Interfaces_Publish(HMODULE owner, const char* name, uint32_t version, const void* table);
const void* Interfaces_Get(HMODULE caller, const char* name, uint32_t minVersion, uint32_t* versionOut);
void Interfaces_FaultOwner(HMODULE owner); // GetInterface stops returning its tables
void Interfaces_Of(HMODULE owner, std::vector<std::string>* published, std::vector<std::string>* used);
void Interfaces_Log();

// --- CrashReport.cpp ---
// <game>\dk2ml-crash-<time>.log for an unhandled exception, from CreateMiniDump before the game's dump. Dying process:
// no heap, never waits on a lock.
struct CrashText { // fixed buffer, cut off when full
    char* buf;
    size_t cap, len;
    void Add(const char* fmt, ...);
};

struct CrashFrame {
    uintptr_t pc;
    uintptr_t functionStart; // from unwind data (0: none, e.g. leaf or generated code)
    void* returnedThroughPost; // reached through a post hook on this function
};

struct CrashHelpers { // alloc-free; crashtest fakes them
    bool (*describe)(uintptr_t address, char* out, size_t size); // "Function+0x12"; false if unknown
    bool (*ownerOf)(uintptr_t address, char* out, size_t size); // "x.dll (mod "Title", ...)" for a mod's code
    void (*mods)(CrashText* out); // the plugins section
};

int Crash_Walk(const CONTEXT& context, CrashFrame* frames, int max); // innermost first, through post hooks
void Crash_Format(const EXCEPTION_RECORD& record, const CONTEXT& context, bool mainThread, const CrashHelpers& helpers,
                  CrashText* out);
void CrashReport_SetHelpers(const CrashHelpers& helpers);
void CrashReport_Init(const std::wstring& dir); // main thread, startup; prunes old reports
bool CrashReport_Write(const EXCEPTION_POINTERS* pointers); // once per process; false if not written
const wchar_t* CrashReport_LastPath(); // "" if none

// --- GuiThunks.cpp ---
// Plugin GUI callbacks (thunks -> crash-contained calls) and input capture. No game access (guitest).
struct GuiCallbackLayout {
    int32_t actionOwner; // GUI::sAction::owner (-1: unknown)
    int32_t actionCursor; // GUI::sAction::eventParams + GUI::sEventParams::cursor (a Vector2; -1: unknown)
};

void Gui_SetCallbackLayout(const GuiCallbackLayout& layout);
void Gui_SetCrashCallback(void (*onCrash)(HMODULE owner)); // switches the plugin off
void* Gui_CallbackThunk(HMODULE owner, DK2ML_GuiCallbackFn fn, void* user); // pCallback's value; nullptr if full
bool Gui_IsThunk(const void* address);
int Gui_CallbackCount(HMODULE owner); // distinct callbacks
void Gui_FaultOwner(HMODULE owner); // callbacks go inert, capture released
DK2ML_Status Gui_SetCapture(HMODULE owner, bool capture);
bool Gui_AnyCapture();
bool Gui_Captures(HMODULE owner);

// --- GuiKit.cpp ---
// DK2ML_API::Gui*, the game-event consumer, the game window and version
void GuiKit_Init(); // after Symbols_Init: resolves names
void GuiKit_OnGuiLoaded(bool screen); // each GUIManager::Load: registers the consumer again (screen: 219 too)
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
// The loader's own game hooks (frame tick, GUI load, map load, input capture, resize), owned by the loader.
void GameHooks_Init();
bool GameHooks_Install(void* target, DK2ML_PreFn pre, DK2ML_PostFn post, const char* what); // logs a failure
int64_t GameHooks_GameState(void** gameClient); // GameClient::m_state (-1: unknown), and the GameClient
int GameHooks_IsGameMenuOpen(); // GameGUI::IsAnyMenuOpened without plugins' captures (0 if unknown)

// --- gui/nativemods/NativeModsButton.cpp ---
// The main-menu Modloader button and screen, merged into the GUI as it loads
bool Menu_Init(bool hooksReady); // hooksReady: frame tick and GUI load hooked
bool Menu_Ready();
void Menu_OnFrame();
void Menu_OnGuiLoadBegin();
void Menu_OnGuiLoadEnd();
void Menu_OnGuiLoaded();
