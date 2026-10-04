// PDB explorer and plugin dry run (shipped in the template zip's tools\): the loader's symbol lookups against
// DoorKickers2.exe/.pdb, outside the game.
// usage: symtest.exe [<game dir>] [<plugin.dll>]
//   always: checks the loader's own names and prints the game version
//   with a plugin: dry-runs its DK2ML_PluginInit with real lookups and stubbed hooks (nothing is patched). Exit code:
//   init's result, or 100 if init returned 0 but made calls the real loader would refuse.
// explorer: symtest.exe <game dir> --find <mask> | --types <mask> | --type <name> | --enum <name>
//   functions/globals (decorated name per overload), type names, a type's layout, an enum's values
// symtest.exe <game dir> --check-index [samples]: the loader's name index vs dbghelp's per-call answers
#include "../loader/Loader.h"
#include "../loader/gui/GameUi.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <map>
#include <set>

namespace {

const wchar_t* g_gameDir = L"";
int g_problems = 0; // calls the real loader would refuse
std::map<void*, int> g_safeHooks; // target -> callback count, the loader's own included

void Problem(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("  [dry run] PROBLEM: ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
    ++g_problems;
}

DK2ML_Status StubCreateSafeHook(void* target, DK2ML_PreFn, DK2ML_PostFn, void*)
{
    if (!target) {
        Problem("CreateSafeHook(NULL)");
        return DK2ML_ERROR;
    }

    ++g_safeHooks[target];
    Symbols_WarnIfShared(target, "plugin");
    return DK2ML_OK;
}

DK2ML_Status StubToggle(void* target)
{
    if (!g_safeHooks.count(target)) {
        Problem("EnableHook/DisableHook(%p): no safe hook was created on it", target);
        return DK2ML_ERROR;
    }
    return DK2ML_OK;
}

DK2ML_Status StubAddOption(const DK2ML_Option* option)
{
    DK2ML_Option copy;
    if (const char* error = Options_Check(option, &copy)) {
        Problem("AddOption \"%s\": %s", copy.label ? copy.label : "", error);
        return DK2ML_ERROR;
    }

    bool integer = copy.type == DK2ML_OPTION_INT;
    bool numeric = integer || copy.type == DK2ML_OPTION_FLOAT;
    bool hasFormat = copy.format && copy.format[0];
    if (numeric && hasFormat) {
        std::string used = Options_SafeFormat(copy.format, integer);
        if (used != copy.format) {
            printf(
                "  [dry run] WARNING: AddOption \"%s\": format \"%s\" isn't one plain %s conversion; \"%s\" is used\n",
                copy.label ? copy.label : "", copy.format, integer ? "%d" : "%f/%g/%e", used.c_str());
        }
    }
    return DK2ML_OK;
}

int g_subscriptions = 0;

DK2ML_Status StubSubscribe(DK2ML_EventType type, DK2ML_EventFn fn, void*)
{
    if (const char* error = Events_Check(static_cast<int>(type), fn)) {
        Problem("Subscribe(%d): %s", static_cast<int>(type), error);
        return DK2ML_ERROR;
    }

    ++g_subscriptions;
    return DK2ML_OK;
}

std::set<std::string> g_interfaces; // published in this dry run

DK2ML_Status StubPublishInterface(const char* name, uint32_t version, const void* table)
{
    if (const char* error = Interfaces_Check(name, table)) {
        Problem("PublishInterface: %s", error);
        return DK2ML_ERROR;
    }

    if (!g_interfaces.insert(name).second) {
        Problem("PublishInterface(\"%s\"): published twice", name);
        return DK2ML_ERROR;
    }
    printf("  [dry run] offers interface \"%s\" v%u\n", name, version);
    return DK2ML_OK;
}

const void* StubGetInterface(const char* name, uint32_t, uint32_t* versionOut)
{
    if (versionOut) {
        *versionOut = 0;
    }

    // the dry run only calls init, where GetInterface is always NULL
    Problem("GetInterface(\"%.63s\") during init always returns NULL: look it up from the PLUGINS_LOADED event",
            name ? name : "");
    return nullptr;
}

int g_tasks = 0;

DK2ML_Status StubAddTask(DK2ML_TaskFn fn, void*)
{
    if (!fn) {
        Problem("AddTask(NULL)");
        return DK2ML_ERROR;
    }

    ++g_tasks; // never run: a dry run has no frames
    return DK2ML_OK;
}

DK2ML_Status StubSubscribeGuiEvent(uint32_t id, DK2ML_EventFn fn, void*)
{
    if (const char* error = Events_CheckGui(id, fn)) {
        Problem("SubscribeGuiEvent(%u): %s", id, error);
        return DK2ML_ERROR;
    }

    int64_t count = 0;
    bool countKnown = Symbols_EnumValue("GUI::Events::eEventType", "NUM_VALUES", &count);
    if (countKnown && id >= count) {
        Problem("SubscribeGuiEvent(%u): this game build's GUI events end at %lld", id,
                static_cast<long long>(count) - 1);
        return DK2ML_ERROR;
    }

    ++g_subscriptions;
    return DK2ML_OK;
}

// No GUI in a dry run (init runs before the game loads it): lookups find nothing, actions fail.
int g_guiCalls = 0;

void* StubGuiFind(void*, const char*)
{
    ++g_guiCalls;
    return nullptr;
}

void* StubGuiParent(void*)
{
    ++g_guiCalls;
    return nullptr;
}

const char* StubGuiName(void*)
{
    ++g_guiCalls;
    return "";
}

int StubGuiChildren(void*, void**, int)
{
    ++g_guiCalls;
    return 0;
}

int StubGuiIsShown(void*)
{
    ++g_guiCalls;
    return 0;
}

void StubGuiShow(void*, int)
{
    ++g_guiCalls;
}

DK2ML_Status StubGuiSetText(void*, const char*)
{
    ++g_guiCalls;
    return DK2ML_ERROR;
}

DK2ML_Status StubGuiAddChild(void*, void*)
{
    ++g_guiCalls;
    return DK2ML_ERROR;
}

DK2ML_Status StubGuiSetOrigin(void*, float, float)
{
    ++g_guiCalls;
    return DK2ML_ERROR;
}

DK2ML_Status StubGuiClick(void*)
{
    ++g_guiCalls;
    return DK2ML_ERROR;
}

DK2ML_Status StubGuiSetCallback(void*, int, DK2ML_GuiCallbackFn, void*)
{
    ++g_guiCalls;
    return DK2ML_ERROR;
}

void TestLog(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("  [plugin] ");
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
}

const wchar_t* TestConfigDir()
{
    static std::wstring dir;
    if (dir.empty()) {
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        dir = std::wstring(temp) + L"dk2ml_symtest\\";
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir.c_str();
}

// Field by field, independent of order. The assert catches a new DK2ML_API field missing here.
static_assert(sizeof(DK2ML_API) == offsetof(DK2ML_API, IsGameMenuOpen) + sizeof(void*),
              "DK2ML_API grew: fill the new fields");

DK2ML_API MakeTestApi()
{
    DK2ML_API api = {};
    api.apiVersion = DK2ML_API_VERSION;
    api.structSize = sizeof(DK2ML_API);
    api.ResolveSymbol = [](const char* n) { return Symbols_Resolve(n); };
    api.GetFieldOffset = [](const char* t, const char* f) { return Symbols_FieldOffset(t, f); };
    api.GetTypeSize = [](const char* t) { return Symbols_TypeSize(t); };
    api.EnableHook = StubToggle;
    api.DisableHook = StubToggle;
    api.Log = TestLog;
    api.GetGameDir = []() { return g_gameDir; };
    api.IsGameFocused = []() { return 0; };
    api.GetEnumValue = [](const char* t, const char* n, int64_t* o) {
        return Symbols_EnumValue(t, n, o) ? DK2ML_OK : DK2ML_ERROR;
    };
    api.CreateSafeHook = StubCreateSafeHook;
    api.GetConfigDir = TestConfigDir;
    api.AddOption = StubAddOption;
    api.Subscribe = StubSubscribe;
    api.GetGameState = []() -> int64_t { return -1; }; // no game running
    api.PublishInterface = StubPublishInterface;
    api.GetInterface = StubGetInterface;
    api.AddTask = StubAddTask;
    api.GuiFind = StubGuiFind;
    api.GuiParent = StubGuiParent;
    api.GuiName = StubGuiName;
    api.GuiChildren = StubGuiChildren;
    api.GuiIsShown = StubGuiIsShown;
    api.GuiShow = StubGuiShow;
    api.GuiSetText = StubGuiSetText;
    api.GuiAddChild = StubGuiAddChild;
    api.GuiSetOrigin = StubGuiSetOrigin;
    api.GuiClick = StubGuiClick;
    api.GuiSetCallback = StubGuiSetCallback;
    api.CaptureGameInput = [](int) { return DK2ML_OK; };
    api.IsGameMenuOpen = []() { return 0; };
    api.SubscribeGuiEvent = StubSubscribeGuiEvent;
    api.GetGameVersion = []() { return gameui::FindGameVersion(); };
    api.GetGameWindow = []() -> void* { return nullptr; }; // no window in a dry run
    return api;
}

// --- explorer: the PDB without LLVM or text dumps ---

std::string Narrow(const wchar_t* w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// one width, so the names line up
const char* KindLabel(const FoundSymbol& f)
{
    if (f.kind == FoundSymbol::Function) {
        return "function";
    }
    if (f.kind == FoundSymbol::Global) {
        return "global  ";
    }
    return "public  ";
}

int FindSymbols(const std::string& mask)
{
    constexpr size_t kLimit = 300;
    std::vector<FoundSymbol> found = Symbols_Find(mask.c_str(), kLimit);

    std::map<std::string, int> addressesPerName;
    for (const auto& f : found) {
        ++addressesPerName[f.name];
    }

    for (size_t i = 0; i < found.size() && i < kLimit; ++i) {
        const FoundSymbol& f = found[i];
        printf("%s  %-60s", KindLabel(f), f.name.c_str());
        if (addressesPerName[f.name] > 1) {
            printf("  OVERLOAD: ResolveSymbol(\"%s\")", f.decorated.empty() ? "?" : f.decorated.c_str());
        }
        if (f.foldedWith > 1) {
            printf("  FOLDED with %d other function(s): a hook runs for all", f.foldedWith - 1);
        }
        printf("\n");
    }

    if (found.size() > kLimit) {
        printf("... more than %zu matches: narrow the mask\n", kLimit);
    } else if (found.empty()) {
        printf("no function or global matches \"%s\" (masks use * and ?, e.g. \"GameClient::*Camera*\")\n",
               mask.c_str());
    }
    return found.empty() ? 1 : 0;
}

int ListTypes(const std::string& mask)
{
    std::vector<std::string> names = Symbols_FindTypes(mask.c_str());
    for (const auto& n : names) {
        printf("%s\n", n.c_str());
    }

    if (names.empty()) {
        printf("no type matches \"%s\" (masks use * and ?, e.g. \"*Camera*\")\n", mask.c_str());
    }
    return names.empty() ? 1 : 0;
}

int DescribeType(const std::string& name)
{
    std::vector<TypeMember> members;
    uint64_t size = 0;
    int others = 0;
    if (!Symbols_DescribeType(name.c_str(), &members, &size, &others)) {
        printf("no struct/class/union named \"%s\" (try --types \"*%s*\")\n", name.c_str(), name.c_str());
        return 1;
    }

    printf("%s: %llu bytes", name.c_str(), static_cast<unsigned long long>(size));
    if (others) {
        printf(" (the PDB has %d other layout(s) of it, from another build; GetFieldOffset uses this one)", others);
    }

    printf("\n  offset  size  type                                     name\n");
    for (const auto& m : members) {
        printf("  %+6d  %4llu  %-40s %s", m.offset, static_cast<unsigned long long>(m.size), m.type.c_str(),
               m.name.c_str());
        if (m.bitfield) {
            printf("  (bits %u-%u)", m.bitPosition, m.bitPosition + m.bitLength - 1);
        }
        printf("\n");
    }

    printf("Base class members are listed under the base: --type <base>.\n");
    return 0;
}

int ListEnum(const std::string& name)
{
    std::vector<std::pair<std::string, int64_t>> values;
    if (!Symbols_EnumList(name.c_str(), &values)) {
        printf("no enum named \"%s\" (try --types \"*%s*\")\n", name.c_str(), name.c_str());
        return 1;
    }

    for (const auto& [valueName, value] : values) {
        printf("  %-50s %lld\n", valueName.c_str(), static_cast<long long>(value));
    }
    return 0;
}

// The name index (Symbols.cpp) vs dbghelp's slow per-call answers; about a minute.
int CheckIndex(const std::string& arg)
{
    size_t samples = arg.empty() ? 150 : strtoul(arg.c_str(), nullptr, 10);
    std::vector<std::string> names = {
        "ImGui::Render",   "GUIManager::Load",         "GUIManager::MergeItemsFromXML", "Camera::SetDefaults",
        "CreateMiniDump",  "GUI::Item::FindChild",     "GUI::sAction::Execute",         "GUI::Item::Show",
        "GUI::Item::Hide", "GameClient::UpdateCamera", "GameGUI::IsAnyMenuOpened"};

    LogEchoToStdout(true);
    int mismatches = Symbols_CheckIndex(names, samples);

    if (mismatches == 0) {
        printf("index check: all match\n");
        return 0;
    }
    printf("index check: MISMATCHES, see above\n");
    return 1;
}

int Explore(const std::wstring& command, const std::string& arg)
{
    if (command == L"--find") {
        return FindSymbols(arg);
    }
    if (command == L"--types") {
        return ListTypes(arg);
    }
    if (command == L"--type") {
        return DescribeType(arg);
    }
    if (command == L"--enum") {
        return ListEnum(arg);
    }
    if (command == L"--check-index") {
        return CheckIndex(arg);
    }

    printf(
        "unknown option; use --find <mask>, --types <mask>, --type <name>, --enum <name> or --check-index [samples]\n");
    return 2;
}

// --- the loader's own names, checked on every run ---

const char* OkOrMissing(bool ok)
{
    return ok ? "ok" : "MISSING";
}

// a few lookups of each kind: a broken PDB shows at once
void PrintSampleLookups()
{
    const char* syms[] = {"GameClient::UpdateCamera",
                          "?FindChild@Item@GUI@@QEAAPEAV12@PEBD@Z",
                          "GameClient::ConvertScreenToMapCoords",
                          "Camera::UpdateViewMatrix",
                          "GUI::sAction::Execute",
                          "g_pGUIManager"};
    for (auto s : syms) {
        printf("%-45s %p\n", s, Symbols_Resolve(s));
    }

    printf("GUI::sAction size %u, action @%d\n", Symbols_TypeSize("GUI::sAction"),
           Symbols_FieldOffset("GUI::sAction", "action"));

    // one of each plugin warning: overload, bitfield (_UNWIND_INFO::Flags: bits 3-7 of byte 0)
    printf("%-45s %p\n", "GUI::Item::FindChild (overloaded)", Symbols_Resolve("GUI::Item::FindChild"));
    printf("_UNWIND_INFO::Flags (bitfield) @%d\n", Symbols_FieldOffset("_UNWIND_INFO", "Flags"));

    for (auto e : {"ACTION_ADD_CHILD", "ACTION_SHOW"}) {
        int64_t v = -1;
        bool ok = Symbols_EnumValue("GUI::eAction", e, &v);
        printf("GUI::eAction::%s ok=%d value=%lld\n", e, ok, v);
    }
}

void CheckLoaderMenu()
{
    bool menuOk = gameui::Resolve();
    const char* menuStatus = menuOk ? "ok" : "MISSING, see symtest.log";
    printf("loader menu symbols: %s (Button::m_pStaticText @%d, sEventParams::iParam1 @%d, sAction size %u, "
           "XMLDocument size %u, GUI_GAME_last=%lld)\n",
           menuStatus, gameui::off.buttonTexts, gameui::off.eventParamsInt, gameui::off.actionSize,
           gameui::off.xmlDocumentSize, gameui::off.eventGameLast);
    if (!menuOk) {
        return;
    }

    // a folded hook target would run the menu code for unrelated calls
    int shared = Symbols_WarnIfShared(gameui::fn.imguiRender, "loader") +
                 Symbols_WarnIfShared(gameui::fn.guiLoad, "loader") +
                 Symbols_WarnIfShared(reinterpret_cast<void*>(gameui::fn.MergeItemsFromXML), "loader");
    printf("loader hook targets: %s\n", shared == 3 ? "each is a single function" : "FOLDED, see above");

    // in the game, the loader's own hooks are in these chains too
    for (void* own :
         {gameui::fn.imguiRender, gameui::fn.guiLoad, reinterpret_cast<void*>(gameui::fn.MergeItemsFromXML)}) {
        ++g_safeHooks[own];
    }
}

void CheckEvents()
{
    gameui::ResolveEvents();
    const gameui::EventNames& ev = gameui::ev;
    printf("events: FRAME %s, GUI_LOADED %s, STATE_CHANGED %s, MAP_LOADED %s (GameClient::m_state @%d, m_camera @%d)\n",
           OkOrMissing(ev.frame), OkOrMissing(ev.guiLoaded), OkOrMissing(ev.state), OkOrMissing(ev.mapLoaded),
           ev.clientState, ev.clientCamera);

    if (ev.cameraSetDefaults && Symbols_WarnIfShared(ev.cameraSetDefaults, "loader") > 1) {
        printf("events: Camera::SetDefaults is FOLDED with other functions, see above\n");
    }
}

// the crash report hooks the game's top-level exception filter (DllMain.cpp)
void CheckCrashHandler()
{
    void* crashHandler = Symbols_Resolve("CreateMiniDump");

    const char* status = "ok";
    if (!crashHandler) {
        status = "MISSING (no dk2ml-crash-*.log in this build)";
    } else if (Symbols_WarnIfShared(crashHandler, "loader") > 1) {
        status = "FOLDED, see above";
    }
    printf("crash reports: CreateMiniDump %s\n", status);
}

void CheckServices()
{
    bool kitOk = gameui::ResolveKit();
    gameui::ResolveServices();
    const gameui::ServiceNames& svc = gameui::svc;

    bool inputOk = svc.isAnyMenuOpened && svc.gameGui;
    bool guiEventsOk = svc.eventSystem && svc.RegisterConsumer;
    printf("GUI kit: %s; input capture: %s; GUI events: %s (%lld ids); WINDOW_RESIZED: %s\n", OkOrMissing(kitOk),
           OkOrMissing(inputOk), OkOrMissing(guiEventsOk), static_cast<long long>(svc.guiEventCount),
           OkOrMissing(svc.onWindowResized));

    if (svc.isAnyMenuOpened) {
        if (Symbols_WarnIfShared(svc.isAnyMenuOpened, "loader") > 1) {
            printf("input capture: GameGUI::IsAnyMenuOpened is FOLDED with other functions, see above\n");
        }
        ++g_safeHooks[svc.isAnyMenuOpened]; // the loader safe-hooks it whenever plugins run
    }
    if (svc.onWindowResized && Symbols_WarnIfShared(svc.onWindowResized, "loader") > 1) {
        printf("WINDOW_RESIZED: GameRenderer::OnWindowResized is FOLDED with other functions, see above\n");
    }

    printf("game version: %u\n", gameui::FindGameVersion());
}

// The game's active mod list (Plugins_SyncEnabled). Empty here: the exe is only mapped.
void CheckModList(HMODULE exe)
{
    bool modListOk = gameui::ResolveModList();
    std::vector<std::string> active;
    bool listReadable = modListOk && gameui::ActiveModPaths(&active);

    unsigned long long instanceRva = 0;
    if (modListOk) {
        instanceRva = static_cast<unsigned long long>(static_cast<char*>(gameui::modList.instance) -
                                                      reinterpret_cast<char*>(exe));
    }
    printf("mod list: %s (g_modsInstance RVA 0x%llX, m_activeMods @%d, %u-byte entries)\n",
           listReadable ? "ok" : "MISSING (the loader re-reads options.xml instead)", instanceRva,
           gameui::modList.activeMods, gameui::modList.entrySize);
}

void CheckLoaderNames(HMODULE exe)
{
    PrintSampleLookups();
    CheckLoaderMenu();
    CheckEvents();
    CheckCrashHandler();
    CheckServices();
    CheckModList(exe);
}

// --- the dry run ---

// Read from the file as the loader does, before any plugin code runs.
void CheckManifest(const std::wstring& plugin)
{
    PluginManifest manifest;
    Consent_ReadPlugin(plugin, &manifest);
    if (!manifest.present) {
        printf("  [dry run] no manifest (optional: DK2ML_PLUGIN_MANIFEST names it on the Native mods screen)\n");
        return;
    }

    printf("  [dry run] manifest: %s, needs API %u, game version %u%s%s\n", Consent_ManifestLine(manifest).c_str(),
           manifest.minApiVersion, manifest.gameVersion, manifest.url.empty() ? "" : ", ", manifest.url.c_str());

    if (manifest.structSize != sizeof(DK2ML_Manifest)) {
        Problem("manifest structSize is %u, not sizeof(DK2ML_Manifest) = %zu (use DK2ML_PLUGIN_MANIFEST)",
                manifest.structSize, sizeof(DK2ML_Manifest));
    }
    if (manifest.name.empty()) {
        Problem("manifest has no name");
    }
    if (manifest.minApiVersion > DK2ML_API_VERSION) {
        Problem("manifest needs API %u: this loader (%s, API %d) would refuse to load it", manifest.minApiVersion,
                DK2ML_VERSION, DK2ML_API_VERSION);
    }
}

// Returns init's result, or 100 if init returned 0 but made calls the real loader would refuse.
int DryRun(const std::wstring& gameDir, const std::wstring& plugin)
{
    g_gameDir = gameDir.c_str();
    HMODULE mod = LoadLibraryW(plugin.c_str());
    auto init = mod ? reinterpret_cast<DK2ML_PluginInitFn>(GetProcAddress(mod, DK2ML_PLUGIN_INIT_NAME)) : nullptr;
    if (!init) {
        printf("cannot load %ls\n", plugin.c_str());
        return 1;
    }

    std::wstring dir = plugin.substr(0, plugin.find_last_of(L'\\') + 1);
    DK2ML_PluginInfo info = {plugin.c_str(), dir.c_str(), L""};
    const DK2ML_API api = MakeTestApi();
    CheckManifest(plugin);

    int r = init(&api, &info);

    if (g_subscriptions) {
        printf("  [dry run] %d event subscription(s)\n", g_subscriptions);
    }
    if (g_tasks) {
        printf("  [dry run] %d task(s) queued (they run from the first frame in the game)\n", g_tasks);
    }
    if (g_guiCalls) {
        printf("  [dry run] %d GUI kit call(s) during init: there is no GUI yet (use GUI_LOADED)\n", g_guiCalls);
    }

    printf("plugin init returned %d\n", r);
    if (g_problems) {
        printf("dry run: %d call(s) the real loader would refuse, see PROBLEM above\n", g_problems);
    }

    if (r != 0) {
        return r;
    }
    return g_problems ? 100 : 0;
}

} // namespace

int wmain(int argc, wchar_t** argv)
{
    std::wstring gameDir = argc > 1 ? argv[1] : L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\DoorKickers2";
    if (gameDir.back() != L'\\') {
        gameDir += L'\\';
    }

    HMODULE exe = LoadLibraryExW((gameDir + L"DoorKickers2.exe").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!exe) {
        printf("cannot map exe (%lu)\n", GetLastError());
        return 1;
    }

    LogOpen(L"symtest.log");
    bool explore = argc > 2 && wcsncmp(argv[2], L"--", 2) == 0;
    // warnings (duplicate types, folded code) echo too, except in the explorer
    LogEchoToStdout(!explore);
    RealDbghelp_Load();
    if (!Symbols_Init(gameDir, exe)) {
        printf("Symbols_Init failed, see symtest.log\n");
        return 1;
    }

    if (explore) {
        return Explore(argv[2], argc > 3 ? Narrow(argv[3]) : std::string());
    }

    CheckLoaderNames(exe);

    if (argc > 2) {
        return DryRun(gameDir, argv[2]);
    }
    return 0;
}
