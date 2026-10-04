// Checks loader/safety/Consent.cpp with a scripted prompt instead of the dialog: folder classification, mod titles,
// the Mods-menu diff, fingerprints, remembered answers, one prompt for every new or changed item, plugin and manifest
// detection, dependency DLL clashes.
// usage: consenttest.exe   (prints "all passed", exit code 0)
#include "../loader/Loader.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <fstream>

namespace {

int g_failures = 0;
int g_prompts = 0;
bool g_answer = true;
std::vector<ConsentRequest> g_lastPending;

bool ScriptedPrompt(const std::vector<ConsentRequest>& pending)
{
    ++g_prompts;
    g_lastPending = pending;
    return g_answer;
}

void Expect(const char* test, bool ok)
{
    printf("%-64s %s\n", test, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

void WriteFile(const std::wstring& path, const char* content)
{
    std::ofstream(path, std::ios::binary) << content;
}

// one Consent_DecideAll call
struct Decision {
    std::vector<bool> load;
    int prompts;
};

Decision Decide(const std::wstring& ini, std::vector<ConsentRequest> items, bool answer)
{
    g_answer = answer;
    int before = g_prompts;
    Decision d;
    Consent_DecideAll(ini, items, ScriptedPrompt, &d.load);
    d.prompts = g_prompts - before;
    return d;
}

void DeleteTree(const std::wstring& dir)
{
    std::wstring from = dir;
    if (!from.empty() && from.back() == L'\\') {
        from.pop_back();
    }
    from.push_back(L'\0'); // SHFileOperation wants a double-terminated list
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

// the test's folders: a fake Workshop item with a native\ folder, and the ini next to it
struct Paths {
    std::wstring base;
    std::wstring mod;
    std::wstring native;
    std::wstring ini;
};

Paths SetUp()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);

    Paths p;
    p.base = Consent_Canonical(temp) + L"dk2ml_consenttest\\";
    DeleteTree(p.base);
    p.mod = p.base + L"steamapps\\workshop\\content\\1239080\\123456\\";
    p.native = p.mod + L"native\\";
    SHCreateDirectoryExW(nullptr, (p.native + L"data\\").c_str(), nullptr);
    p.ini = p.base + L"dk2ml.ini";
    LogOpen(p.base + L"consenttest.log");

    WriteFile(p.native + L"a.dll", "first plugin v1");
    WriteFile(p.native + L"B.dll", "helper v1");
    WriteFile(p.native + L"defaults.ini", "x=1");
    WriteFile(p.native + L"data\\payload.bin", "not a dll, still code-ish");
    WriteFile(p.mod + L"mod.xml", "<Mod title=\"Test Mod\" author=\"x\"/>");
    return p;
}

void TestIniAndTitles(const Paths& p)
{
    Consent_EnsureIni(p.ini);
    Expect("default dk2ml.ini is created", GetFileAttributesW(p.ini.c_str()) != INVALID_FILE_ATTRIBUTES);
    Expect("default ini keeps the loader enabled", GetPrivateProfileIntW(L"loader", L"enabled", 0, p.ini.c_str()) == 1);
    Expect("mod title is read from mod.xml", Consent_ModTitle(p.mod) == L"Test Mod");

    std::wstring titled = p.base + L"titled\\";
    SHCreateDirectoryExW(nullptr, titled.c_str(), nullptr);
    WriteFile(titled + L"mod.xml", "<Mod subtitle=\"no\" data-title=\"no\" title = \"Spaced &amp; fine\"/>");
    Expect("mod title: only the title attribute, spaces allowed", Consent_ModTitle(titled) == L"Spaced & fine");

    // the game writes titles with entities: the Workshop Free Camera's mod.xml says title="Era&apos;s Free Camera"
    WriteFile(titled + L"mod.xml", "<Mod title=\"Era&apos;s Free Camera\"/>");
    Expect("mod title: entities are decoded", Consent_ModTitle(titled) == L"Era's Free Camera");
    WriteFile(titled + L"mod.xml", "<Mod title=\"A&#39;b &#x2019; &amp;apos; &bogus; & &lt;x&gt; &quot;q&quot;\"/>");
    Expect("mod title: numeric entities, decoded once, unknown ones kept",
           Consent_ModTitle(titled) == L"A'b \x2019 &apos; &bogus; & <x> \"q\"");

    std::string huge = "<Mod title=\"" + std::string(4 << 20, 'x') + "\"/>";
    WriteFile(titled + L"mod.xml", huge.c_str());
    Expect("mod title: a huge mod.xml neither crashes nor gets read whole", Consent_ModTitle(titled) == L"(untitled)");

    huge = "<Mod title=\"" + std::string(60000, 'y') + "\"/>";
    WriteFile(titled + L"mod.xml", huge.c_str());
    Expect("mod title: a long title is cut to 100 characters", Consent_ModTitle(titled) == std::wstring(100, L'y'));
}

void TestWorkshopRoot(const std::wstring& ws)
{
    Expect("workshop root comes from the game's Steam library",
           Consent_WorkshopRoot(L"E:\\SteamLibrary\\steamapps\\common\\DoorKickers2\\") == ws);
    Expect("no workshop root outside a Steam library", Consent_WorkshopRoot(L"D:\\Games\\DoorKickers2\\").empty());
    Expect("no workshop root for a folder nested in steamapps\\common",
           Consent_WorkshopRoot(L"E:\\SteamLibrary\\steamapps\\common\\DoorKickers2\\bin\\").empty());
}

void TestClassification(const Paths& p)
{
    std::vector<std::wstring> roots = {L"C:\\Users\\u\\AppData\\Local\\KillHouseGames\\DoorKickers2\\mods\\",
                                       L"D:\\Games\\DoorKickers2\\mods_native\\"};
    std::wstring ws = L"E:\\SteamLibrary\\steamapps\\workshop\\content\\1239080\\";
    std::wstring id;
    TestWorkshopRoot(ws);

    Expect("local mod folder is local",
           Consent_Classify(L"C:\\Users\\u\\AppData\\Local\\KillHouseGames\\DoorKickers2\\mods\\cool\\", roots, ws,
                            &id) == ModSource::Local);
    Expect("local root itself is not a mod",
           Consent_Classify(L"C:\\Users\\u\\AppData\\Local\\KillHouseGames\\DoorKickers2\\mods\\", roots, ws, &id) ==
               ModSource::Unknown);

    id.clear();
    ModSource item =
        Consent_Classify(L"E:\\SteamLibrary\\steamapps\\workshop\\content\\1239080\\3141592\\", roots, ws, &id);
    Expect("workshop item is classified with its id", item == ModSource::Workshop && id == L"3141592");

    Expect("lookalike workshop path outside the game's library is unknown",
           Consent_Classify(L"D:\\Downloads\\steamapps\\workshop\\content\\1239080\\3141592\\", roots, ws, &id) ==
               ModSource::Unknown);
    Expect("no workshop root: nothing is a workshop item",
           Consent_Classify(L"E:\\SteamLibrary\\steamapps\\workshop\\content\\1239080\\3141592\\", roots, L"", &id) ==
               ModSource::Unknown);
    Expect("another game's workshop item is unknown",
           Consent_Classify(L"E:\\SteamLibrary\\steamapps\\workshop\\content\\440\\3141592\\", roots, ws, &id) ==
               ModSource::Unknown);
    Expect("non-numeric workshop id is unknown",
           Consent_Classify(L"E:\\SteamLibrary\\steamapps\\workshop\\content\\1239080\\abc\\", roots, ws, &id) ==
               ModSource::Unknown);
    Expect("folder nested inside a workshop item is unknown",
           Consent_Classify(L"E:\\SteamLibrary\\steamapps\\workshop\\content\\1239080\\31\\x\\", roots, ws, &id) ==
               ModSource::Unknown);
    Expect("anything else is unknown", Consent_Classify(L"C:\\Temp\\evil\\", roots, ws, &id) == ModSource::Unknown);
    Expect("case doesn't matter",
           Consent_Classify(L"c:\\users\\U\\appdata\\local\\killhousegames\\doorkickers2\\MODS\\cool\\", roots, ws,
                            &id) == ModSource::Local);

    Expect("canonical path of a real folder ends in a backslash", !p.mod.empty() && Consent_Canonical(p.mod) == p.mod);

    // the game stores mod folders with mixed slashes (options.xml: ...\AppData\Local/KillHouseGames/...)
    std::wstring mixed = p.mod;
    for (size_t i = mixed.size() / 2; i < mixed.size(); ++i) {
        if (mixed[i] == L'\\') {
            mixed[i] = L'/';
        }
    }
    mixed.pop_back(); // and without the trailing separator
    Expect("canonical path of a mixed-slash spelling is the same folder", Consent_Canonical(mixed) == p.mod);
}

void TestXmlDecode()
{
    Expect("xml decode: malformed or out of range is kept as text",
           Consent_XmlDecode("&#0; &#x110000; &#xD800; &#12 &amp &#; &#xZZ; &#128512;") ==
               "&#0; &#x110000; &#xD800; &#12 &amp &#; &#xZZ; \xF0\x9F\x98\x80");
}

// the Mods menu during a session
void TestModsMenuDiff()
{
    std::vector<std::wstring> known = {L"C:\\Mods\\A\\", L"C:\\Mods\\B\\", L"E:\\ws\\1239080\\31\\"};

    EnabledDiff same = Consent_DiffEnabled(known, {L"C:/Mods/A", L"c:\\mods\\b\\", L"E:\\ws\\1239080\\31"});
    Expect("mod list: unchanged, whatever the slashes and case",
           same.active == std::vector<bool>({true, true, true}) && same.added.empty());

    EnabledDiff off = Consent_DiffEnabled(known, {L"E:\\ws\\1239080\\31\\", L"C:\\Mods\\A\\"});
    Expect("mod list: one turned off, order doesn't matter",
           off.active == std::vector<bool>({true, false, true}) && off.added.empty());

    EnabledDiff added = Consent_DiffEnabled(known, {L"C:\\Mods\\A\\", L"C:/Mods/New", L"C:\\mods\\new\\",
                                                    L"C:\\Mods\\B\\", L"E:\\ws\\1239080\\31\\", L"D:\\Other/X"});
    bool addedInOrder =
        added.added.size() == 2 && added.added[0] == L"C:\\Mods\\New\\" && added.added[1] == L"D:\\Other\\X\\";
    Expect("mod list: new folders once each, in order, slashes fixed",
           added.active == std::vector<bool>({true, true, true}) && addedInOrder);

    EnabledDiff none = Consent_DiffEnabled(known, {});
    Expect("mod list: all turned off", none.active == std::vector<bool>({false, false, false}) && none.added.empty());

    EnabledDiff back = Consent_DiffEnabled({L"C:\\Mods\\A\\", L""}, {L"", L"C:\\Mods\\A\\"});
    Expect("mod list: empty entries match nothing",
           back.active == std::vector<bool>({true, false}) && back.added.empty());

    Expect("mod list key: backslashes, trailing backslash, lowercase",
           Consent_ModListKey(L"C:\\Users\\U\\AppData\\Local/KillHouseGames/DoorKickers2/mods_upload/Eras") ==
               L"c:\\users\\u\\appdata\\local\\killhousegames\\doorkickers2\\mods_upload\\eras\\");
}

// returns the fingerprint of the native folder as SetUp wrote it
std::wstring TestFingerprints(const Paths& p)
{
    std::wstring f1 = Consent_Fingerprint(p.native);
    Expect("fingerprint is a sha256 hex string", f1.size() == 64);

    WriteFile(p.native + L"data\\payload.bin", "changed");
    std::wstring f2 = Consent_Fingerprint(p.native);
    Expect("fingerprint covers non-DLL files in subfolders", f2.size() == 64 && f2 != f1);

    WriteFile(p.native + L"data\\payload.bin", "not a dll, still code-ish");
    Expect("fingerprint is stable", Consent_Fingerprint(p.native) == f1);
    Expect("fingerprint of a missing folder is empty", Consent_Fingerprint(p.base + L"missing\\").empty());
    return f1;
}

void TestRememberedAnswers(const Paths& p, const std::wstring& f1)
{
    ConsentRequest item{L"123456", L"Test Mod", p.native};

    Decision d = Decide(p.ini, {item}, true);
    bool askedAsNew = g_lastPending.size() == 1 && !g_lastPending[0].changed && g_lastPending[0].fingerprint == f1;
    Expect("first time: asks, and Yes loads", d.prompts == 1 && d.load == std::vector<bool>({true}) && askedAsNew);

    d = Decide(p.ini, {item}, false);
    Expect("allowed version: loads without asking", d.prompts == 0 && d.load[0]);

    WriteFile(p.native + L"B.dll", "helper v2");
    d = Decide(p.ini, {item}, false);
    Expect("changed DLL: asks again (marked updated), No skips",
           d.prompts == 1 && !d.load[0] && g_lastPending[0].changed);

    d = Decide(p.ini, {item}, true);
    Expect("declined version: skipped without asking", d.prompts == 0 && !d.load[0]);

    WriteFile(p.native + L"defaults.ini", "x=2");
    d = Decide(p.ini, {item}, true);
    Expect("declined, then any file changed: asks again", d.prompts == 1 && d.load[0] && g_lastPending[0].changed);
}

// several items at once: one prompt for the new and changed ones; answered and unreadable ones are left out
void TestSeveralItems(const Paths& p)
{
    ConsentRequest item{L"123456", L"Test Mod", p.native};
    std::wstring otherNative = p.base + L"other\\native\\";
    SHCreateDirectoryExW(nullptr, otherNative.c_str(), nullptr);
    WriteFile(otherNative + L"o.dll", "other plugin v1");
    ConsentRequest other{L"777", L"Other", otherNative};
    ConsentRequest unreadable{L"888", L"Unreadable", p.base + L"missing\\native\\"};
    WriteFile(p.native + L"defaults.ini", "x=3");

    Decision d = Decide(p.ini, {item, other, unreadable}, true);
    bool listsBoth = g_lastPending.size() == 2 && g_lastPending[0].itemId == L"123456" && g_lastPending[0].changed &&
                     g_lastPending[1].itemId == L"777" && !g_lastPending[1].changed;
    Expect("one prompt lists every new or changed item", d.prompts == 1 && listsBoth);
    Expect("Yes loads them all; an unreadable item never loads", d.load == std::vector<bool>({true, true, false}));

    wchar_t stored[256] = {};
    GetPrivateProfileStringW(L"workshop", L"888", L"", stored, 256, p.ini.c_str());
    Expect("an unreadable item gets no answer", stored[0] == 0);

    d = Decide(p.ini, {item, other}, false);
    Expect("all answered: no prompt", d.prompts == 0 && d.load == std::vector<bool>({true, true}));

    std::wstring text = Consent_PromptText(g_lastPending);
    bool namesBoth = text.find(L"\"Test Mod\"  (updated)  workshop item 123456") != std::wstring::npos &&
                     text.find(L"\"Other\"  (new)  workshop item 777") != std::wstring::npos;
    Expect("prompt text: each item's title, new or updated, and id", namesBoth);
    Expect("prompt text: the warning", text.find(L"full access to your PC") != std::wstring::npos);

    WriteFile(p.native + L"defaults.ini", "x=4");
    WriteFile(otherNative + L"o.dll", "other plugin v2");
    d = Decide(p.ini, {item, other}, false);
    Expect("No skips them all", d.prompts == 1 && d.load == std::vector<bool>({false, false}));

    d = Decide(p.ini, {item, other}, true);
    Expect("No is remembered for each until it changes", d.prompts == 0 && d.load == std::vector<bool>({false, false}));

    std::vector<ConsentRequest> many;
    for (int i = 0; i < 20; ++i) {
        many.push_back({std::to_wstring(1000 + i), L"Mod " + std::to_wstring(i), otherNative});
    }
    std::wstring long_ = Consent_PromptText(many);
    bool cutWithCount = long_.find(L"\"Mod 14\"") != std::wstring::npos &&
                        long_.find(L"\"Mod 15\"") == std::wstring::npos &&
                        long_.find(L"and 5 more") != std::wstring::npos;
    Expect("prompt text: a long list is cut with a count", cutWithCount);
}

// which DLLs are plugins: decided from the export table, without running anything
void TestPluginDetection(const Paths& p)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring buildDir(self);
    buildDir = buildDir.substr(0, buildDir.find_last_of(L'\\') + 1);

    wchar_t sys[MAX_PATH];
    wchar_t wow[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    GetSystemWow64DirectoryW(wow, MAX_PATH);

    std::wstring junk = p.base + L"junk.dll";
    WriteFile(junk, "MZ not really a dll");
    std::wstring empty = p.base + L"empty.dll";
    WriteFile(empty, "");

    Expect("the example plugin is a plugin", Consent_IsPlugin(buildDir + L"example_plugin.dll"));
    Expect("a DLL without DK2ML_PluginInit is not", !Consent_IsPlugin(std::wstring(sys) + L"\\version.dll"));
    Expect("a 32-bit DLL is not", !Consent_IsPlugin(std::wstring(wow) + L"\\version.dll"));
    Expect("a broken file named .dll is not", !Consent_IsPlugin(junk) && !Consent_IsPlugin(empty));
    Expect("a missing file is not", !Consent_IsPlugin(p.base + L"missing.dll"));
    Expect("checking didn't load it as a module", GetModuleHandleW(L"example_plugin.dll") == nullptr);

    // the manifest (DK2ML_PluginManifest), read from the same mapping
    PluginManifest m;
    bool plugin = Consent_ReadPlugin(buildDir + L"example_plugin.dll", &m);
    Expect("manifest: the example's is read as written",
           plugin && m.present && m.structSize == sizeof(DK2ML_Manifest) && m.minApiVersion == 1 &&
               m.name == "Example plugin" && m.version == "1.0.0" && m.author == "dk2ml" && m.url.empty() &&
               m.gameVersion == 112);
    Expect("manifest: the line players see", Consent_ManifestLine(m) == "\"Example plugin\" 1.0.0 by dk2ml");

    // each read refills m, so these checks stay one && chain in this order
    Expect("manifest: a plugin without one is still a plugin",
           Consent_ReadPlugin(buildDir + L"nomanifest_plugin.dll", &m) && !m.present &&
               Consent_ManifestLine(m).empty());

    plugin = Consent_ReadPlugin(buildDir + L"manifest_plugin.dll", &m);
    Expect("manifest: a hostile one is cut to its array and cleaned",
           plugin && m.present && m.minApiVersion == 99 && m.name.size() == 64 && m.name.compare(0, 6, "Name X") == 0 &&
               m.name.back() == 'E' && m.version == "9.9" && m.author == "Someone Else" && m.gameVersion == 0);
    Expect("manifest: none in a non-plugin, a broken or a missing file",
           !Consent_ReadPlugin(std::wstring(sys) + L"\\version.dll", &m) && !m.present &&
               !Consent_ReadPlugin(junk, &m) && !m.present && !Consent_ReadPlugin(p.base + L"missing.dll", &m) &&
               !m.present);

    bool nothingLoaded =
        GetModuleHandleW(L"manifest_plugin.dll") == nullptr && GetModuleHandleW(L"example_plugin.dll") == nullptr;
    Expect("reading manifests didn't load anything", nothingLoaded);
}

// dependency DLLs two mods ship under one name
void TestDllClashes(const Paths& p)
{
    std::wstring a = p.base + L"dup_a.dll";
    std::wstring b = p.base + L"dup_b.dll";
    std::wstring c = p.base + L"dup_c.dll";
    WriteFile(a, "version 1");
    WriteFile(b, "version 2");
    WriteFile(c, "version 1");

    std::wstring ha = Consent_FileHash(a);
    std::wstring hb = Consent_FileHash(b);
    std::wstring hc = Consent_FileHash(c);
    Expect("file hash: same content same hash, other content other hash", ha.size() == 64 && ha == hc && ha != hb);
    Expect("a missing file has no hash", Consent_FileHash(p.base + L"none.dll").empty());

    std::vector<DllClash> clashes = Consent_FindDllClashes(
        {{0, L"helper.dll", ha}, {1, L"HELPER.dll", hb}, {2, L"helper.dll", hc}, {0, L"other.dll", hb}});
    bool pairs = clashes.size() == 2 && clashes[0].modA == 0 && clashes[0].modB == 1 && clashes[1].modA == 1 &&
                 clashes[1].modB == 2;
    Expect("same name (any case), other content: a clash per pair of mods", pairs);
    Expect("the same file in two mods is fine", Consent_FindDllClashes({{0, L"x.dll", ha}, {1, L"x.dll", hc}}).empty());
    Expect("an unreadable copy counts as a clash",
           Consent_FindDllClashes({{0, L"x.dll", L""}, {1, L"x.dll", L""}}).size() == 1);
}

} // namespace

int main()
{
    Paths p = SetUp();

    TestIniAndTitles(p);
    TestClassification(p);
    TestXmlDecode();
    TestModsMenuDiff();
    std::wstring f1 = TestFingerprints(p);
    TestRememberedAnswers(p, f1);
    TestSeveralItems(p);
    TestPluginDetection(p);
    TestDllClashes(p);

    DeleteTree(p.base);
    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
