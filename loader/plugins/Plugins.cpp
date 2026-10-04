// Finds and loads plugin DLLs from enabled mods and <game>\mods_native\.
#include "Loader.h"

#include <intrin.h>
#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <vector>

#include "MinHook.h"

namespace {

std::wstring g_gameDir;

std::wstring WithSlash(std::wstring p)
{
    for (auto& c : p) {
        if (c == L'/') {
            c = L'\\';
        }
    }
    if (!p.empty() && p.back() != L'\\') {
        p += L'\\';
    }
    return p;
}

std::wstring Lower(std::wstring s)
{
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring Utf8ToWide(const std::string& s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string ToUtf8(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring SaveDir()
{
    PWSTR localAppData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData))) {
        dir = std::wstring(localAppData) + L"\\KillHouseGames\\DoorKickers2\\";
    }
    CoTaskMemFree(localAppData);
    return dir;
}

constexpr char kModsTag[] = "<Mods";
constexpr size_t kModsTagLength = sizeof(kModsTag) - 1;
constexpr char kPathAttribute[] = "path"; // path0, path1, ...
constexpr size_t kPathAttributeLength = sizeof(kPathAttribute) - 1;

// Start of <Mods>'s attributes, or npos. Hand-scanned: MSVC's std::regex recurses per character.
size_t FindModsAttributes(const std::string& xml)
{
    for (size_t at = xml.find(kModsTag); at != std::string::npos; at = xml.find(kModsTag, at + 1)) {
        size_t afterName = at + kModsTagLength;
        char next = afterName < xml.size() ? xml[afterName] : '\0';
        bool nameEnds = next == '>' || next == '/' || isspace(static_cast<unsigned char>(next));
        if (nameEnds) {
            return afterName;
        }
    }
    return std::string::npos;
}

// options.xml's mod folders, in load order.
std::vector<std::wstring> EnabledModDirs()
{
    std::vector<std::wstring> dirs;
    std::wstring optionsPath = SaveDir() + L"options.xml";
    std::ifstream file(optionsPath, std::ios::binary);
    if (!file) {
        LogF("cannot read %ls, no mod folders will be scanned", optionsPath.c_str());
        return dirs;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    std::string xml = ss.str();

    // <Mods path0="..." path1="..." .../>
    size_t start = FindModsAttributes(xml);
    if (start == std::string::npos) {
        return dirs;
    }
    size_t end = xml.find('>', start);
    std::string tag = xml.substr(start, end == std::string::npos ? std::string::npos : end - start);

    for (size_t at = tag.find(kPathAttribute); at != std::string::npos; at = tag.find(kPathAttribute, at + 1)) {
        if (at > 0 && !isspace(static_cast<unsigned char>(tag[at - 1]))) {
            continue;
        }

        // pathN="value"
        size_t i = at + kPathAttributeLength;
        if (i >= tag.size() || !isdigit(static_cast<unsigned char>(tag[i]))) {
            continue;
        }
        while (i < tag.size() && isdigit(static_cast<unsigned char>(tag[i]))) {
            ++i;
        }
        if (tag.compare(i, 2, "=\"") != 0) {
            continue;
        }
        size_t close = tag.find('"', i + 2);
        if (close == std::string::npos) {
            break;
        }

        dirs.push_back(WithSlash(Utf8ToWide(Consent_XmlDecode(tag.substr(i + 2, close - i - 2)))));
        at = close;
    }
    return dirs;
}

std::vector<std::wstring> DllsIn(const std::wstring& dir)
{
    std::vector<std::wstring> dlls;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return dlls;
    }

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            dlls.push_back(dir + fd.cFileName);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return dlls;
}

// manifests parallel to plugins
void SplitDlls(const std::wstring& dir, std::vector<std::wstring>* plugins, std::vector<std::wstring>* support,
               std::vector<PluginManifest>* manifests)
{
    for (auto& dll : DllsIn(dir)) {
        PluginManifest manifest;
        if (Consent_ReadPlugin(dll, &manifest)) {
            plugins->push_back(dll);
            manifests->push_back(manifest);
        } else {
            support->push_back(dll);
        }
    }
}

HMODULE ModuleAt(void* address)
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &module);
    return module;
}

std::wstring ModuleFileName(HMODULE module)
{
    return Log_ModuleName(module);
}

std::string NarrowName(HMODULE module)
{
    return ToUtf8(ModuleFileName(module));
}

// --- the menu's records ---

std::vector<ModEntry> g_mods;
std::vector<OptionEntry*> g_options; // never freed: onChange gets pointers into them
// Written only during init; the screen reads it lock-free. The lock is for plugin threads and FaultOptionsOf.
SRWLOCK g_optionsLock = SRWLOCK_INIT;
bool g_optionsOpen = false;

std::wstring g_ini;

ModEntry* EntryOf(HMODULE module)
{
    for (auto& e : g_mods) {
        for (HMODULE m : e.modules) {
            if (m == module) {
                return &e;
            }
        }
    }
    return nullptr;
}

void MarkStatus(HMODULE module, ModStatus status)
{
    if (ModEntry* e = EntryOf(module)) {
        e->status = status;
    }
}

void FaultOptionsOf(HMODULE owner)
{
    AcquireSRWLockShared(&g_optionsLock);
    for (OptionEntry* o : g_options) {
        if (o->owner == owner) {
            o->faulted = true;
        }
    }
    ReleaseSRWLockShared(&g_optionsLock);
}

// --- hooks by owner, so a failing plugin is switched off as a whole ---

void DisableHooksOf(HMODULE owner)
{
    // even mid-call; other owners' callbacks keep running
    SafeHook_FaultOwner(owner);
    Events_FaultOwner(owner);      // its events and waiting tasks
    Interfaces_FaultOwner(owner);  // its tables point into switched-off code
    Gui_FaultOwner(owner);         // GUI callbacks inert, input capture released
}

// Switched off for the session. Their threads may still call the API, so CreateSafeHook and EnableHook refuse them.
SRWLOCK g_offLock = SRWLOCK_INIT;
std::vector<HMODULE> g_switchedOff;

bool IsSwitchedOff(HMODULE owner)
{
    AcquireSRWLockShared(&g_offLock);
    bool off = std::find(g_switchedOff.begin(), g_switchedOff.end(), owner) != g_switchedOff.end();
    ReleaseSRWLockShared(&g_offLock);
    return off;
}

void SwitchOff(HMODULE owner)
{
    AcquireSRWLockExclusive(&g_offLock);
    if (std::find(g_switchedOff.begin(), g_switchedOff.end(), owner) == g_switchedOff.end()) {
        g_switchedOff.push_back(owner);
    }
    ReleaseSRWLockExclusive(&g_offLock);

    DisableHooksOf(owner);
    FaultOptionsOf(owner);
}

DK2ML_Status Refused(HMODULE owner, const char* what, void* target)
{
    LogF("%ls: %s(%p) refused: it was switched off for this session", ModuleFileName(owner).c_str(), what, target);
    return DK2ML_ERROR;
}

void OnSafeHookFault(HMODULE owner, DWORD, void*)
{
    LogF("%ls crashed and was switched off for this session (the game continues without it)",
         ModuleFileName(owner).c_str());
    SwitchOff(owner);
    MarkStatus(owner, ModStatus::Crashed);
}

// Events.cpp or GuiThunks.cpp already logged it
void OnEventCrash(HMODULE owner)
{
    LogF("%ls was switched off for this session (the game continues without it)", ModuleFileName(owner).c_str());
    SwitchOff(owner);
    MarkStatus(owner, ModStatus::Crashed);
}

// --- names a plugin looked up that this build lacks ---

constexpr size_t kMaxMissingPerPlugin = 32; // bounds a plugin probing names in a loop
SRWLOCK g_missingLock = SRWLOCK_INIT;
std::vector<std::pair<HMODULE, std::string>> g_missing;

void RecordMissing(HMODULE owner, std::string what)
{
    AcquireSRWLockExclusive(&g_missingLock);
    size_t count = 0;
    bool known = false;
    for (const auto& [m, w] : g_missing) {
        if (m == owner) {
            ++count;
            known |= w == what;
        }
    }
    if (!known && count < kMaxMissingPerPlugin) {
        g_missing.push_back({owner, std::move(what)});
    }
    ReleaseSRWLockExclusive(&g_missingLock);
}

std::vector<std::string> MissingOf(HMODULE owner)
{
    std::vector<std::string> names;
    AcquireSRWLockShared(&g_missingLock);
    for (const auto& [m, w] : g_missing) {
        if (m == owner) {
            names.push_back(w);
        }
    }
    ReleaseSRWLockShared(&g_missingLock);
    return names;
}

std::string JoinNames(const std::vector<std::string>& names)
{
    std::string s;
    for (const auto& n : names) {
        s += (s.empty() ? "" : ", ") + n;
    }
    return s;
}

// --- API exposed to plugins ---

constexpr size_t kMaxLookupName = 256; // bytes kept of a looked-up name

std::string Name(const char* s)
{
    return s ? std::string(s, strnlen(s, kMaxLookupName)) : std::string("(null)");
}

std::string Copy(const char* s)
{
    return s ? std::string(s, strnlen(s, kMaxOptionText)) : std::string();
}

__declspec(noinline) void* Api_ResolveSymbol(const char* name)
{
    void* address = Symbols_Resolve(name);
    if (!address) {
        RecordMissing(ModuleAt(_ReturnAddress()), Name(name));
    }
    return address;
}

__declspec(noinline) int32_t Api_GetFieldOffset(const char* typeName, const char* fieldName)
{
    int32_t offset = Symbols_FieldOffset(typeName, fieldName);
    if (offset < 0) {
        RecordMissing(ModuleAt(_ReturnAddress()), Name(typeName) + "::" + Name(fieldName));
    }
    return offset;
}

__declspec(noinline) uint32_t Api_GetTypeSize(const char* typeName)
{
    uint32_t size = Symbols_TypeSize(typeName);
    if (!size) {
        RecordMissing(ModuleAt(_ReturnAddress()), "type " + Name(typeName));
    }
    return size;
}

// Only the caller's callbacks. No safe hook on the target: refused, so plugins can't toggle the loader's hooks.
DK2ML_Status SetHookEnabled(HMODULE owner, void* target, bool enable)
{
    int r = SafeHook_SetEnabled(target, owner, enable);
    if (r == 0) {
        LogF("%ls: %s(%p): no safe hook on that function", ModuleFileName(owner).c_str(),
             enable ? "EnableHook" : "DisableHook", target);
    }
    return r > 0 ? DK2ML_OK : DK2ML_ERROR;
}

__declspec(noinline) DK2ML_Status Api_EnableHook(void* target)
{
    HMODULE owner = ModuleAt(_ReturnAddress());
    if (IsSwitchedOff(owner)) {
        return Refused(owner, "EnableHook", target);
    }
    return SetHookEnabled(owner, target, true);
}

__declspec(noinline) DK2ML_Status Api_DisableHook(void* target)
{
    return SetHookEnabled(ModuleAt(_ReturnAddress()), target, false);
}

__declspec(noinline) void Api_Log(const char* fmt, ...)
{
    char prefix[MAX_PATH] = "plugin";
    if (HMODULE caller = ModuleAt(_ReturnAddress())) {
        char path[MAX_PATH];
        if (GetModuleFileNameA(caller, path, MAX_PATH)) {
            const char* base = strrchr(path, '\\');
            strncpy_s(prefix, base ? base + 1 : path, _TRUNCATE);
        }
    }

    va_list args;
    va_start(args, fmt);
    LogLine(prefix, fmt, args);
    va_end(args);
}

const wchar_t* Api_GetGameDir()
{
    return g_gameDir.c_str();
}

int Api_IsGameFocused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

__declspec(noinline) DK2ML_Status Api_GetEnumValue(const char* enumType, const char* name, int64_t* out)
{
    if (Symbols_EnumValue(enumType, name, out)) {
        return DK2ML_OK;
    }
    RecordMissing(ModuleAt(_ReturnAddress()), Name(enumType) + "::" + Name(name));
    return DK2ML_ERROR;
}

__declspec(noinline) DK2ML_Status Api_CreateSafeHook(void* target, DK2ML_PreFn pre, DK2ML_PostFn post, void* user)
{
    HMODULE owner = ModuleAt(_ReturnAddress());
    if (IsSwitchedOff(owner)) { // a new callback wouldn't be marked faulted, so it would run
        return Refused(owner, "CreateSafeHook", target);
    }

    DK2ML_Status s = SafeHook_Create(target, pre, post, user, owner);
    if (s == DK2ML_OK) {
        Symbols_WarnIfShared(target, NarrowName(owner).c_str());
    }
    return s;
}

// Creates <save>\dk2ml\<dll name without extension>\; "" if it can't.
std::wstring MakeConfigDir(HMODULE caller)
{
    std::wstring name = ModuleFileName(caller);
    name = name.substr(0, name.find_last_of(L'.'));
    std::wstring dir;
    std::wstring save = SaveDir();
    if (save.empty()) {
        return dir;
    }

    dir = save + L"dk2ml\\" + name + L"\\";
    int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    bool exists = r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
    if (!exists) {
        LogF("cannot create %ls (error %d)", dir.c_str(), r);
        dir.clear();
    }
    return dir;
}

// Outside the mod folder, which Steam replaces on every Workshop update.
__declspec(noinline) const wchar_t* Api_GetConfigDir()
{
    static SRWLOCK lock = SRWLOCK_INIT;
    static std::vector<std::pair<HMODULE, std::wstring>> dirs; // never freed
    HMODULE caller = ModuleAt(_ReturnAddress());

    AcquireSRWLockExclusive(&lock);
    for (auto& [module, dir] : dirs) {
        if (module == caller) {
            ReleaseSRWLockExclusive(&lock);
            return dir.c_str();
        }
    }

    dirs.push_back({caller, MakeConfigDir(caller)});
    const wchar_t* result = dirs.back().second.c_str();
    ReleaseSRWLockExclusive(&lock);
    return result;
}

__declspec(noinline) DK2ML_Status Api_AddOption(const DK2ML_Option* plugin)
{
    HMODULE owner = ModuleAt(_ReturnAddress());
    if (!g_optionsOpen) {
        LogF("%ls: AddOption after DK2ML_PluginInit returned, ignored "
             "(options are shown from the start: add them in init)",
             ModuleFileName(owner).c_str());
        return DK2ML_ERROR;
    }
    DK2ML_Option copy;
    if (const char* error = Options_Check(plugin, &copy)) {
        LogF("%ls: AddOption \"%s\" ignored: %s", ModuleFileName(owner).c_str(), Copy(copy.label).c_str(), error);
        return DK2ML_ERROR;
    }

    // copies: the plugin's strings needn't outlive the call
    const DK2ML_Option* option = &copy;
    auto* o = new OptionEntry{owner, Copy(option->label), Copy(option->tooltip), Copy(option->format), {}, {}, *option,
                              false};
    if (option->type == DK2ML_OPTION_CHOICE) {
        for (int i = 0; i < option->choiceCount; ++i) {
            o->choices.push_back(Copy(option->choices[i]));
        }
    }
    for (auto& c : o->choices) {
        o->choicePointers.push_back(c.c_str());
    }

    // what onChange gets points into the copies
    o->api.structSize = sizeof(DK2ML_Option);
    o->api.label = o->label.c_str();
    o->api.tooltip = o->tooltip.c_str();
    o->api.format = o->format.c_str();
    o->api.choices = o->choicePointers.empty() ? nullptr : o->choicePointers.data();

    AcquireSRWLockExclusive(&g_optionsLock);
    g_options.push_back(o);
    ReleaseSRWLockExclusive(&g_optionsLock);
    return DK2ML_OK;
}

__declspec(noinline) DK2ML_Status Api_Subscribe(DK2ML_EventType type, DK2ML_EventFn fn, void* user)
{
    return Events_Subscribe(ModuleAt(_ReturnAddress()), static_cast<int>(type), fn, user);
}

int64_t Api_GetGameState()
{
    return GameHooks_GameState(nullptr);
}

__declspec(noinline) DK2ML_Status Api_PublishInterface(const char* name, uint32_t version, const void* table)
{
    return Interfaces_Publish(ModuleAt(_ReturnAddress()), name, version, table);
}

__declspec(noinline) const void* Api_GetInterface(const char* name, uint32_t minVersion, uint32_t* versionOut)
{
    return Interfaces_Get(ModuleAt(_ReturnAddress()), name, minVersion, versionOut);
}

__declspec(noinline) DK2ML_Status Api_AddTask(DK2ML_TaskFn fn, void* user)
{
    return Events_AddTask(ModuleAt(_ReturnAddress()), fn, user);
}

// the GUI kit is in GuiKit.cpp; only GuiSetCallback needs the owner
__declspec(noinline) DK2ML_Status Api_GuiSetCallback(void* item, int itemEvent, DK2ML_GuiCallbackFn fn, void* user)
{
    return Gui_SetCallback(ModuleAt(_ReturnAddress()), item, itemEvent, fn, user);
}

__declspec(noinline) DK2ML_Status Api_CaptureGameInput(int capture)
{
    return Gui_SetCapture(ModuleAt(_ReturnAddress()), capture != 0);
}

int Api_IsGameMenuOpen()
{
    return GameHooks_IsGameMenuOpen();
}

__declspec(noinline) DK2ML_Status Api_SubscribeGuiEvent(uint32_t guiEventId, DK2ML_EventFn fn, void* user)
{
    return Events_SubscribeGui(ModuleAt(_ReturnAddress()), guiEventId, fn, user);
}

uint32_t Api_GetGameVersion()
{
    return GuiKit_GameVersion();
}

void* Api_GetGameWindow()
{
    return GuiKit_GameWindow();
}

// positional: a new DK2ML_API field without an entry here would be NULL
static_assert(sizeof(DK2ML_API) == offsetof(DK2ML_API, IsGameMenuOpen) + sizeof(void*),
              "DK2ML_API grew: add the new fields");
const DK2ML_API g_api = {
    DK2ML_API_VERSION,
    sizeof(DK2ML_API),
    Api_ResolveSymbol,
    Api_GetFieldOffset,
    Api_GetTypeSize,
    Api_GetEnumValue,
    Api_CreateSafeHook,
    Api_EnableHook,
    Api_DisableHook,
    Api_Log,
    Api_GetGameDir,
    Api_GetConfigDir,
    Api_IsGameFocused,
    Api_GetGameState,
    Api_GetGameVersion,
    Api_GetGameWindow,
    Api_AddOption,
    Api_Subscribe,
    Api_SubscribeGuiEvent,
    Api_PublishInterface,
    Api_GetInterface,
    Api_AddTask,
    Gui_Find,
    Gui_Parent,
    Gui_Name,
    Gui_Children,
    Gui_IsShown,
    Gui_Show,
    Gui_SetText,
    Gui_AddChild,
    Gui_SetOrigin,
    Gui_Click,
    Api_GuiSetCallback,
    Api_CaptureGameInput,
    Api_IsGameMenuOpen,
};

// --- loading ---

struct Candidate {
    std::wstring path;   // the DLL that gets loaded
    std::wstring modDir; // the mod folder it came from; empty for mods_native
    std::wstring title;  // mod.xml title, for messages
    size_t entry;        // index in g_mods
    PluginManifest manifest;
};

// Strings handed to plugins; never freed.
struct LoadedPlugin {
    std::wstring path;
    std::wstring dir;
    std::wstring modDir;
    DK2ML_PluginInfo info;
};

std::vector<LoadedPlugin*> g_loaded;

constexpr int kInitCrashed = INT_MIN;

// No destructors in here (__try).
int CallInit(DK2ML_PluginInitFn init, const DK2ML_PluginInfo* info, DWORD* code)
{
    __try {
        return init(&g_api, info);
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return kInitCrashed;
    }
}

std::string InParentheses(const std::string& text)
{
    return text.empty() ? std::string() : " (" + text + ")";
}

ModStatus LoadPlugin(const Candidate& c, HMODULE* loaded)
{
    // DllMain runs uncontained inside LoadLibraryExW; if it crashes, this is the log's last line.
    LogF("loading %ls", c.path.c_str());
    HMODULE mod =
        LoadLibraryExW(c.path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!mod) {
        LogF("failed to load %ls (error %lu)", c.path.c_str(), GetLastError());
        return ModStatus::LoadFailed;
    }
    auto init = reinterpret_cast<DK2ML_PluginInitFn>(GetProcAddress(mod, DK2ML_PLUGIN_INIT_NAME));
    if (!init) {
        LogF("%ls has no %s export, unloading", c.path.c_str(), DK2ML_PLUGIN_INIT_NAME);
        FreeLibrary(mod);
        return ModStatus::LoadFailed;
    }
    *loaded = mod;

    auto* p = new LoadedPlugin{c.path, c.path.substr(0, c.path.find_last_of(L'\\') + 1), c.modDir, {}};
    p->info = {p->path.c_str(), p->dir.c_str(), p->modDir.c_str()};
    g_loaded.push_back(p);

    DWORD code = 0;
    int result = CallInit(init, &p->info, &code);
    std::vector<std::string> missing = MissingOf(mod);
    if (!missing.empty()) {
        const char* verdict = result == 0 ? " (fine if they're optional)" : ", probably why its init failed";
        LogF("%ls looked up %zu name(s) this game build doesn't have%s: %s", c.path.c_str(), missing.size(), verdict,
             JoinNames(missing).c_str());
    }

    if (result == kInitCrashed) {
        // not unloaded: it may have started threads or hooks
        LogF("%ls crashed during init (exception 0x%08lX); its hooks are switched off", c.path.c_str(), code);
        SwitchOff(mod);
        return ModStatus::Crashed;
    }
    if (result != 0) {
        LogF("%ls init returned %d; its hooks are switched off", c.path.c_str(), result);
        SwitchOff(mod);
        return ModStatus::InitFailed;
    }

    std::string about = Consent_ManifestLine(c.manifest);
    LogF("loaded %ls%s", c.path.c_str(), InParentheses(about).c_str());
    return ModStatus::Loaded;
}

std::string AlsoShippedBy(const std::wstring& dll, const std::wstring& otherTitle)
{
    return ToUtf8(dll) + " is also shipped by " + ToUtf8(otherTitle) + " in another version: only one is used";
}

// Dependency DLLs two mods ship under one name with different contents, or that the game already loaded. Windows
// loads a DLL name once per process, so one plugin gets the other's copy.
void CheckSupportDlls()
{
    std::vector<SupportDll> dlls;
    for (size_t i = 0; i < g_mods.size(); ++i) {
        ModEntry& e = g_mods[i];
        if (e.status != ModStatus::Loaded) { // its code won't load
            continue;
        }

        std::wstring native = e.gameNativeFolder ? e.modDir : e.modDir + L"native\\";
        for (const auto& name : e.supportNames) {
            dlls.push_back({i, name, Consent_FileHash(native + name)});
            if (GetModuleHandleW(name.c_str())) {
                LogF("WARNING: %ls ships %ls, but the game already has a module of that name loaded: "
                     "its plugins will use the game's copy",
                     e.title.c_str(), name.c_str());
                e.conflicts.push_back(ToUtf8(name) +
                                      " is already loaded by the game, so its plugins use the game's copy");
            }
        }
    }

    for (const DllClash& c : Consent_FindDllClashes(dlls)) {
        ModEntry& a = g_mods[c.modA];
        ModEntry& b = g_mods[c.modB];
        LogF("WARNING: %ls and %ls both ship %ls, as different files. Windows loads that name once, "
             "so one of them will run with the other's copy; "
             "tell their authors (a unique name, or linking it in, fixes it)",
             a.title.c_str(), b.title.c_str(), c.name.c_str());
        a.conflicts.push_back(AlsoShippedBy(c.name, b.title));
        b.conflicts.push_back(AlsoShippedBy(c.name, a.title));
    }
}

std::vector<std::wstring> FileNames(const std::vector<std::wstring>& paths)
{
    std::vector<std::wstring> names;
    for (const auto& p : paths) {
        names.push_back(p.substr(p.find_last_of(L'\\') + 1));
    }
    return names;
}

ModEntry NewEntry(const std::wstring& title, const std::wstring& dir, ModSource source, bool gameNativeFolder,
                  const std::vector<std::wstring>& plugins, const std::vector<std::wstring>& support,
                  const std::vector<PluginManifest>& manifests, ModStatus status)
{
    ModEntry e{};
    e.title = title;
    e.modDir = dir;
    e.source = source;
    e.gameNativeFolder = gameNativeFolder;
    e.dllNames = FileNames(plugins);
    e.manifests = manifests;
    e.supportNames = FileNames(support);
    e.status = status;
    e.codeDir = gameNativeFolder ? dir : dir + L"native\\";

    for (const auto& s : support) {
        LogF("%ls doesn't export %s: not a plugin, left for the plugins in its folder to use", s.c_str(),
             DK2ML_PLUGIN_INIT_NAME);
    }
    return e;
}

// --- the Mods menu during a session (Plugins_SyncEnabled) ---

bool g_syncOn = false;       // Plugins_LoadAll ran
bool g_listFromFile = false; // the last sync re-read options.xml

std::wstring g_workshopRoot;
std::vector<std::wstring> g_localRoots;
// every mod folder seen (Consent_ModListKey): never scanned again
std::vector<std::wstring> g_seenKeys;

// the game's list, or options.xml without it
std::vector<std::wstring> CurrentModList(const std::vector<std::string>* gameList)
{
    std::vector<std::wstring> now;
    if (gameList) {
        for (const auto& raw : *gameList) {
            now.push_back(Utf8ToWide(raw)); // the same bytes options.xml gets
        }
    } else {
        now = EnabledModDirs();
    }

    static size_t lastCount = SIZE_MAX;
    bool fromFile = !gameList;
    if (now.size() != lastCount || g_listFromFile != fromFile) {
        LogF("mod list: %zu active mod(s), from %s", now.size(), gameList ? "the game" : "options.xml");
    }
    lastCount = now.size();
    g_listFromFile = fromFile;
    return now;
}

// A folder not looked at yet: enabled since the start, or a known mod under another spelling.
// knownMods: g_mods indexes, parallel to diff->active.
void OnModListed(const std::wstring& listed, const std::vector<size_t>& knownMods, EnabledDiff* diff)
{
    std::wstring key = Consent_ModListKey(listed);
    if (std::find(g_seenKeys.begin(), g_seenKeys.end(), key) != g_seenKeys.end()) {
        return; // seen at the start: no native code, or unresolvable
    }
    g_seenKeys.push_back(key);
    std::wstring modDir = Consent_Canonical(listed);
    if (modDir.empty()) {
        return;
    }

    bool alias = false;
    for (size_t k = 0; k < knownMods.size(); ++k) {
        if (_wcsicmp(g_mods[knownMods[k]].modDir.c_str(), modDir.c_str()) == 0) {
            g_mods[knownMods[k]].listKey = key;
            diff->active[k] = true;
            alias = true;
        }
    }
    if (alias) {
        return;
    }

    std::vector<std::wstring> plugins, support;
    std::vector<PluginManifest> manifests;
    SplitDlls(modDir + L"native\\", &plugins, &support, &manifests); // mapped as data
    if (plugins.empty()) {
        return;
    }

    std::wstring id;
    ModSource source = Consent_Classify(modDir, g_localRoots, g_workshopRoot, &id);
    ModStatus status = source == ModSource::Unknown ? ModStatus::PathNotAllowed : ModStatus::EnabledLater;
    g_mods.push_back(NewEntry(Consent_ModTitle(modDir), modDir, source, /*gameNativeFolder=*/false, plugins, support,
                              manifests, status));
    ModEntry& e = g_mods.back();
    e.workshopId = id;
    e.listKey = key;
    e.seenInGame = true;
    if (status == ModStatus::PathNotAllowed) {
        return;
    }

    LogF("%ls: enabled in the Mods menu after the game started: its native code loads at the next start",
         e.title.c_str());
}

void OnModListState(ModEntry& e, bool active)
{
    if (active) {
        if (!e.enabledNow) {
            const char* note =
                e.status == ModStatus::TurnedOff ? "; it stays switched off until the game restarts" : "";
            LogF("%ls: enabled again in the Mods menu%s", e.title.c_str(), note);
        }
        e.enabledNow = true;
        e.seenInGame = true;
        return;
    }

    // only a mod the game has listed can leave: the list may not be filled yet
    if (!e.seenInGame || !e.enabledNow) {
        return;
    }

    e.enabledNow = false;
    if (e.status != ModStatus::Loaded) {
        LogF("%ls: disabled in the Mods menu", e.title.c_str());
        return;
    }

    // now: the game drops the mod's GUI files and data in this load
    for (HMODULE m : e.modules) {
        SwitchOff(m);
    }
    e.status = ModStatus::TurnedOff;
    LogF("%ls: disabled in the Mods menu, so switched off for this session "
         "(its code stays loaded until the game restarts)",
         e.title.c_str());
}

bool NeedsRestart(const ModEntry& e)
{
    bool enabledLaterAndListed = e.status == ModStatus::EnabledLater && e.enabledNow;
    return e.status == ModStatus::TurnedOff || enabledLaterAndListed;
}

// --- startup: Plugins_LoadAll's steps, in order ---

// A mod's plugins; loading waits for the Workshop answers.
struct Found {
    size_t entry;
    std::vector<std::wstring> plugins;
    std::vector<PluginManifest> manifests;
};

struct Startup {
    std::wstring nativeRoot;              // <game>\mods_native\, canonical ("" if it doesn't exist)
    std::vector<std::wstring> localRoots; // the player's own mod folders, canonical
    std::wstring workshopRoot;            // canonical; empty without subscriptions
    std::vector<Found> found;             // every mod's plugins, in load order
    std::vector<ConsentRequest> workshop; // Workshop items, asked about in one prompt
    std::vector<size_t> workshopEntries;  // their g_mods indexes
};

// Room in g_mods for mods enabled after the start.
constexpr size_t kModsEnabledLater = 64;

void SetLoaderCallbacks()
{
    SafeHook_SetFaultCallback(OnSafeHookFault);
    SafeHook_SetTargetNamer(Symbols_NameAt);
    Events_SetCrashCallback(OnEventCrash);
    Gui_SetCrashCallback(OnEventCrash); // GuiThunks.cpp already logged it
}

// Only the player's own mod folders load without asking. Compared after resolving links: a junction can't fake local.
void FindRoots(const std::wstring& gameDir, const std::wstring& save, Startup* s)
{
    s->nativeRoot = Consent_Canonical(gameDir + L"mods_native\\");
    for (const std::wstring& root : {save + L"mods\\", save + L"mods_upload\\", s->nativeRoot}) {
        std::wstring canonical = root.empty() ? L"" : Consent_Canonical(root);
        if (!canonical.empty()) {
            s->localRoots.push_back(canonical);
        }
    }

    std::wstring gameCanonical = Consent_Canonical(gameDir);
    s->workshopRoot = Consent_Canonical(Consent_WorkshopRoot(gameCanonical)); // empty without subscriptions
    LogF("workshop folder: %ls", s->workshopRoot.empty() ? L"(none)" : s->workshopRoot.c_str());

    // Plugins_SyncEnabled classifies mods enabled later against the same roots
    g_localRoots = s->localRoots;
    g_workshopRoot = s->workshopRoot;
}

void ScanGameNativeFolder(Startup* s)
{
    if (s->nativeRoot.empty()) {
        return;
    }

    std::vector<std::wstring> plugins, support;
    std::vector<PluginManifest> manifests;
    SplitDlls(s->nativeRoot, &plugins, &support, &manifests);
    if (plugins.empty() && !support.empty()) {
        LogF("%ls has DLLs but none exports %s (a 64-bit plugin must): nothing to load", s->nativeRoot.c_str(),
             DK2ML_PLUGIN_INIT_NAME);
    }
    if (!plugins.empty()) {
        g_mods.push_back(NewEntry(L"mods_native (game folder)", s->nativeRoot, ModSource::Local,
                                  /*gameNativeFolder=*/true, plugins, support, manifests, ModStatus::Loaded));
        s->found.push_back({g_mods.size() - 1, plugins, manifests});
    }
}

ModStatus StatusBeforeConsent(ModSource source)
{
    if (source == ModSource::Local) {
        return ModStatus::Loaded;
    }
    if (source == ModSource::Workshop) {
        return ModStatus::Declined; // until the player says yes
    }
    return ModStatus::PathNotAllowed;
}

void ScanListedMods(const std::vector<std::wstring>& enabled, Startup* s)
{
    for (auto& listed : enabled) {
        std::wstring key = Consent_ModListKey(listed);
        g_seenKeys.push_back(key);
        std::wstring modDir = Consent_Canonical(listed);
        if (modDir.empty()) {
            continue;
        }

        // mapped as data: nothing runs
        std::vector<std::wstring> plugins, support;
        std::vector<PluginManifest> manifests;
        SplitDlls(modDir + L"native\\", &plugins, &support, &manifests);
        if (plugins.empty()) {
            if (!support.empty()) {
                LogF("%lsnative\\ has DLLs but none exports %s (a 64-bit plugin must): nothing to load", modDir.c_str(),
                     DK2ML_PLUGIN_INIT_NAME);
            }
            continue;
        }

        std::wstring id;
        ModSource source = Consent_Classify(modDir, s->localRoots, s->workshopRoot, &id);
        ModStatus status = StatusBeforeConsent(source);
        g_mods.push_back(NewEntry(Consent_ModTitle(modDir), modDir, source, /*gameNativeFolder=*/false, plugins,
                                  support, manifests, status));
        size_t index = g_mods.size() - 1;
        g_mods[index].workshopId = id;
        g_mods[index].listKey = key;
        s->found.push_back({index, plugins, manifests});

        if (source == ModSource::Workshop) {
            s->workshop.push_back({id, g_mods[index].title, modDir + L"native\\"});
            s->workshopEntries.push_back(index);
        } else if (source == ModSource::Unknown) {
            LogF("skipping the native code in %ls: neither one of your mod folders nor a Door Kickers 2 Workshop item",
                 modDir.c_str());
        }
    }
}

ModStatus StatusAfterConsent(bool load, const ConsentRequest& request)
{
    if (load) {
        return ModStatus::Loaded;
    }
    if (request.fingerprint.empty()) {
        return ModStatus::Unreadable;
    }
    return ModStatus::Declined;
}

// Workshop code loads only with the player's permission (Consent.cpp)
void DecideWorkshop(bool allowWorkshop, const std::wstring& ini, Startup* s)
{
    if (allowWorkshop) {
        for (size_t k = 0; k < s->workshop.size(); ++k) {
            g_mods[s->workshopEntries[k]].status = ModStatus::Loaded;
            LogF("workshop item %ls (%ls) allowed by allow_workshop_plugins=1", s->workshop[k].itemId.c_str(),
                 s->workshop[k].title.c_str());
        }
    } else if (!s->workshop.empty()) {
        std::vector<bool> load;
        Consent_DecideAll(ini, s->workshop, Consent_Prompt, &load);
        for (size_t k = 0; k < s->workshop.size(); ++k) {
            g_mods[s->workshopEntries[k]].status = StatusAfterConsent(load[k], s->workshop[k]);
        }
    }
}

std::vector<Candidate> BuildCandidates(const std::vector<Found>& found)
{
    std::vector<Candidate> candidates;
    for (const Found& f : found) {
        const ModEntry& e = g_mods[f.entry];
        if (e.status != ModStatus::Loaded) {
            continue;
        }

        // mods_native isn't a mod: no mod folder or title
        std::wstring modDir;
        std::wstring title;
        if (!e.gameNativeFolder) {
            modDir = e.modDir;
            title = e.title;
        }
        for (size_t i = 0; i < f.plugins.size(); ++i) {
            candidates.push_back({f.plugins[i], modDir, title, f.entry, f.manifests[i]});
        }
    }
    return candidates;
}

void SetRegistrationOpen(bool open)
{
    g_optionsOpen = open;
    Events_SetOpen(open);     // subscriptions, like options, only during init
    Interfaces_SetOpen(open); // published only now, looked up only after
}

// One copy per plugin file name: a duplicate would hook everything twice and share one GetConfigDir.
void LoadCandidates(const std::vector<Candidate>& candidates)
{
    std::vector<std::pair<std::wstring, size_t>> loadedNames; // lowercase file name, g_mods index
    for (auto& c : candidates) {
        ModEntry& entry = g_mods[c.entry];
        std::wstring file = c.path.substr(c.path.find_last_of(L'\\') + 1);
        std::wstring key = Lower(file);

        auto sameName = [&](const auto& n) { return n.first == key; };
        auto first = std::find_if(loadedNames.begin(), loadedNames.end(), sameName);
        if (first != loadedNames.end()) {
            entry.status = ModStatus::Duplicate;
            entry.duplicateOf = g_mods[first->second].title;
            LogF("not loading %ls: a plugin named %ls is already loaded from %ls", c.path.c_str(), file.c_str(),
                 entry.duplicateOf.c_str());
            continue;
        }
        if (c.manifest.minApiVersion > DK2ML_API_VERSION) {
            // read from the file: none of its code has run
            entry.status = ModStatus::NeedsNewerLoader;
            entry.neededApi = c.manifest.minApiVersion;
            LogF("not loading %ls: it needs plugin API %u, this loader is %s with API %d", c.path.c_str(),
                 c.manifest.minApiVersion, DK2ML_VERSION, DK2ML_API_VERSION);
            continue;
        }

        HMODULE module = nullptr;
        ModStatus status = LoadPlugin(c, &module);
        if (module) {
            entry.modules.push_back(module);
            loadedNames.push_back({key, c.entry});
            for (auto& name : MissingOf(module)) {
                entry.missing.push_back(name);
            }
        }
        if (status != ModStatus::Loaded) { // one failing DLL marks the mod
            entry.status = status;
        }
    }
}

void FinishLoading()
{
    SetRegistrationOpen(false);
    Interfaces_Log();
    Events_Dispatch(DK2ML_EVENT_PLUGINS_LOADED, nullptr, -1, -1); // plugins find each other's interfaces here
    g_syncOn = true;
}

// --- the shared-hooks log (Plugins_LogSharedHooks) ---

constexpr size_t kMaxNamesListed = 6; // the hooked name and up to 5 names folded into it

std::string OwnerList(const std::vector<HMODULE>& owners, HMODULE self)
{
    std::string list;
    for (HMODULE m : owners) {
        std::string owner = m == self ? std::string("the loader") : NarrowName(m);
        list += (list.empty() ? "" : ", ") + owner;
    }
    return list;
}

// "" unless ICF folded other functions into this one: each plugin's hook runs for all of them
std::string FoldedNote(const std::vector<std::string>& names)
{
    if (names.size() <= 1) {
        return std::string();
    }

    std::string others;
    for (size_t i = 1; i < names.size() && i < kMaxNamesListed; ++i) {
        others += (i > 1 ? ", " : "") + names[i];
    }
    const char* more = names.size() > kMaxNamesListed ? ", ..." : "";
    return " (the same code as " + others + more + ": the linker merged them)";
}

} // namespace

const DK2ML_API* Plugins_Api()
{
    return &g_api;
}

std::vector<ModEntry>& Plugins_Mods()
{
    return g_mods;
}

std::vector<OptionEntry*>& Plugins_Options()
{
    return g_options;
}

const std::wstring& Plugins_Ini()
{
    return g_ini;
}

std::wstring Plugins_SaveDir()
{
    return SaveDir();
}

void Plugins_LogSharedHooks()
{
    // plugins can't see each other's hooks, so log every shared chain's order
    HMODULE self = ModuleAt(reinterpret_cast<void*>(&Plugins_LogSharedHooks));
    for (const auto& chain : SafeHook_Chains()) {
        if (chain.owners.size() < 2) {
            continue;
        }

        std::string owners = OwnerList(chain.owners, self);
        std::vector<std::string> names = Symbols_NamesAt(chain.target);
        std::string folded = FoldedNote(names);
        const char* name = names.empty() ? "?" : names[0].c_str();
        LogF("%s (%p)%s is hooked by %s: pre callbacks run in that order, post callbacks in reverse", name,
             chain.target, folded.c_str(), owners.c_str());
    }
}

// --- the crash report's view of the mods: a dying process, so no allocation ---

namespace {

void NarrowInto(const wchar_t* w, char* out, size_t size)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, static_cast<int>(size), nullptr, nullptr);
    if (n <= 0) {
        out[0] = 0;
    }
    out[size - 1] = 0;
}

const char* StatusName(ModStatus status)
{
    switch (status) {
    case ModStatus::Loaded: return "running";
    case ModStatus::Declined: return "not loaded (the player said No)";
    case ModStatus::PathNotAllowed: return "not loaded (folder not allowed)";
    case ModStatus::InitFailed: return "init failed, switched off";
    case ModStatus::Crashed: return "crashed earlier, switched off";
    case ModStatus::LoadFailed: return "couldn't be loaded";
    case ModStatus::Unreadable: return "not loaded (files unreadable)";
    case ModStatus::Duplicate: return "not loaded (duplicate)";
    case ModStatus::NeedsNewerLoader: return "not loaded (needs a newer loader)";
    case ModStatus::TurnedOff: return "disabled in the Mods menu, switched off";
    case ModStatus::EnabledLater: return "enabled after the start, not loaded";
    }
    return "?";
}

const char* SeparatorBefore(const std::string& text, const char* separator)
{
    return text.empty() ? "" : separator;
}

void ManifestInto(const PluginManifest& m, char* out, size_t size)
{
    if (!m.present) {
        out[0] = 0;
        return;
    }
    _snprintf_s(out, size, _TRUNCATE, " \"%s\"%s%s%s%s", m.name.c_str(), SeparatorBefore(m.version, " "),
                m.version.c_str(), SeparatorBefore(m.author, " by "), m.author.c_str());
}

} // namespace

bool Plugins_CrashOwner(uintptr_t address, char* out, size_t size)
{
    // by file under the mod's code folder: a plugin or a DLL it ships
    void* base = nullptr;
    if (!RtlPcToFileHeader(reinterpret_cast<void*>(address), &base) || !base) {
        return false;
    }
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(static_cast<HMODULE>(base), path, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        return false;
    }

    for (const ModEntry& e : g_mods) {
        if (e.codeDir.empty() || _wcsnicmp(path, e.codeDir.c_str(), e.codeDir.size()) != 0) {
            continue;
        }

        const wchar_t* file = path + e.codeDir.size();
        char fileName[128];
        char title[256];
        char about[256] = "";
        NarrowInto(file, fileName, sizeof(fileName));
        NarrowInto(e.title.c_str(), title, sizeof(title));
        for (size_t k = 0; k < e.dllNames.size() && k < e.manifests.size(); ++k) {
            if (_wcsicmp(e.dllNames[k].c_str(), file) == 0) {
                ManifestInto(e.manifests[k], about, sizeof(about));
            }
        }
        _snprintf_s(out, size, _TRUNCATE, "%s (mod \"%s\"%s%s)", fileName, title, about[0] ? "," : "", about);
        return true;
    }
    return false;
}

void Plugins_CrashMods(CrashText* out)
{
    out->Add("\r\nNative mods (%zu):\r\n", g_mods.size());
    for (const ModEntry& e : g_mods) {
        char title[256];
        NarrowInto(e.title.c_str(), title, sizeof(title));
        out->Add("  \"%s\": %s\r\n", title, StatusName(e.status));

        for (size_t k = 0; k < e.dllNames.size(); ++k) {
            char file[128];
            char about[256] = "";
            NarrowInto(e.dllNames[k].c_str(), file, sizeof(file));
            if (k < e.manifests.size()) {
                ManifestInto(e.manifests[k], about, sizeof(about));
            }
            out->Add("      %s%s\r\n", file, about);
        }
    }
    if (g_mods.empty()) {
        out->Add("  none\r\n");
    }
}

void Plugins_OnOptionCrash(HMODULE owner)
{
    LogF("%ls crashed in an option callback and was switched off for this session", ModuleFileName(owner).c_str());
    SwitchOff(owner);
    MarkStatus(owner, ModStatus::Crashed);
}

void Plugins_LoadAll(const std::wstring& gameDir)
{
    g_gameDir = gameDir;
    g_ini = gameDir + L"dk2ml.ini";
    bool allowWorkshop = GetPrivateProfileIntW(L"loader", L"allow_workshop_plugins", 0, g_ini.c_str()) != 0;
    std::wstring save = SaveDir();
    SetLoaderCallbacks();

    Startup startup;
    FindRoots(gameDir, save, &startup);

    // The crash report reads g_mods from any thread, so it never reallocates: listed + mods_native + later ones.
    std::vector<std::wstring> enabled = EnabledModDirs();
    g_mods.reserve(enabled.size() + 1 + kModsEnabledLater);

    ScanGameNativeFolder(&startup);
    ScanListedMods(enabled, &startup);
    DecideWorkshop(allowWorkshop, g_ini, &startup);

    std::vector<Candidate> candidates = BuildCandidates(startup.found);
    LogF("%zu plugin(s) found", candidates.size());
    CheckSupportDlls();

    SetRegistrationOpen(true);
    LoadCandidates(candidates);
    FinishLoading();
}

void Plugins_SyncEnabled(const std::vector<std::string>* gameList)
{
    if (!g_syncOn) {
        return;
    }

    std::vector<std::wstring> now = CurrentModList(gameList);

    std::vector<std::wstring> known;
    std::vector<size_t> knownMods; // indexes into g_mods, parallel to known
    for (size_t i = 0; i < g_mods.size(); ++i) {
        if (!g_mods[i].listKey.empty()) {
            known.push_back(g_mods[i].listKey);
            knownMods.push_back(i);
        }
    }
    EnabledDiff diff = Consent_DiffEnabled(known, now);

    for (const auto& listed : diff.added) {
        OnModListed(listed, knownMods, &diff);
    }
    for (size_t k = 0; k < knownMods.size(); ++k) {
        OnModListState(g_mods[knownMods[k]], diff.active[k]);
    }
}

bool Plugins_ModListFromFile()
{
    return g_listFromFile;
}

bool Plugins_RestartNeeded()
{
    for (const ModEntry& e : g_mods) {
        if (NeedsRestart(e)) {
            return true;
        }
    }
    return false;
}
