// loader/plugins/Replays.cpp on files in a temporary folder: AddReplayData's rules, the block (round trip, files without
// one, damage, renames), saving after StopRecording into the file it just wrote, loading on RequestReplayStart, the
// nested StopRecording when watching right after a mission (and another replay starting then), the game's path
// spellings, crash containment per plugin, and the loader's own crash.
#include "../loader/Loader.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Expect(const char* test, bool ok)
{
    printf("%-58s %s\n", test, ok ? "ok" : "FAILED");
    if (!ok) {
        ++g_failures;
    }
}

const HMODULE kA = reinterpret_cast<HMODULE>(0x10000); // writes g_alpha
const HMODULE kB = reinterpret_cast<HMODULE>(0x20000); // writes nothing
const HMODULE kC = reinterpret_cast<HMODULE>(0x30000); // crashes in save
const HMODULE kD = reinterpret_cast<HMODULE>(0x40000); // writes too much
const HMODULE kE = reinterpret_cast<HMODULE>(0x50000); // crashes in load

std::string g_alpha = "alpha";
std::string g_loadedA; // what A's load got
bool g_loadedANull = false;
int g_loadsA = 0;
int g_loadsB = 0;
bool g_loadedBNull = false;
HMODULE g_crashed = nullptr;
int g_crashes = 0;
DK2ML_Status g_overflowResult = DK2ML_OK;
DK2ML_Status g_nullDataResult = DK2ML_OK;

void OnCrash(HMODULE owner)
{
    g_crashed = owner;
    ++g_crashes;
}

void SaveA(DK2ML_ReplayWriter* out, void*)
{
    Replays_Write(kA, out, g_alpha.data(), 2); // two writes add up
    Replays_Write(kA, out, g_alpha.data() + 2, g_alpha.size() - 2);
}

void LoadA(const void* data, size_t size, void*)
{
    ++g_loadsA;
    g_loadedANull = data == nullptr && size == 0;
    g_loadedA = data ? std::string(static_cast<const char*>(data), size) : "";
}

void SaveNothing(DK2ML_ReplayWriter*, void*) {}

void LoadB(const void* data, size_t size, void*)
{
    ++g_loadsB;
    g_loadedBNull = data == nullptr && size == 0;
}

void SaveCrash(DK2ML_ReplayWriter*, void*)
{
    *static_cast<volatile int*>(nullptr) = 1;
}

void LoadNothing(const void*, size_t, void*) {}

void SaveTooMuch(DK2ML_ReplayWriter* out, void*)
{
    std::string big(DK2ML_REPLAY_MAX_DATA, 'x');
    g_nullDataResult = Replays_Write(kD, out, nullptr, 1);
    Replays_Write(kD, out, big.data(), big.size()); // exactly the cap: fine
    g_overflowResult = Replays_Write(kD, out, "y", 1);
}

void LoadCrash(const void*, size_t, void*)
{
    *static_cast<volatile int*>(nullptr) = 1;
}

DK2ML_ReplayData Data(const char* key, void (*save)(DK2ML_ReplayWriter*, void*),
                      void (*load)(const void*, size_t, void*))
{
    return {sizeof(DK2ML_ReplayData), key, save, load, nullptr};
}

void DeleteFilesIn(const std::wstring& dir)
{
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW((dir + L"*.rpl").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        std::wstring path = dir + data.cFileName;
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL); // a read-only one from a run that stopped midway
        DeleteFileW(path.c_str());
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

// Emptied: a run starts from no replays.
std::wstring TempDir()
{
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::wstring dir = std::wstring(temp) + L"dk2ml_replaystest\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"replays\\").c_str(), nullptr);
    DeleteFilesIn(dir);
    DeleteFilesIn(dir + L"replays\\");
    return dir;
}

// A replay as the game writes it: a 0xF8 byte header, two chunks, a zero length.
void WriteFakeReplay(const std::wstring& path)
{
    std::string bytes(0xF8, 'h');
    for (const char* chunk : {"first packet", "second"}) {
        uint16_t length = static_cast<uint16_t>(strlen(chunk));
        bytes.append(reinterpret_cast<const char*>(&length), sizeof(length));
        bytes += chunk;
    }
    bytes.append(2, '\0');

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

void SetWriteTime(const std::wstring& path, const FILETIME& time)
{
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    SetFileTime(file, nullptr, nullptr, &time);
    CloseHandle(file);
}

// Back-dated, so a file written after it counts as newer.
void MakeOld(const std::wstring& path)
{
    SetWriteTime(path, {0, 0x01D00000}); // 2014
}

uint64_t SizeOf(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

void Truncate(const std::wstring& path, uint64_t size)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(size);
    SetFilePointerEx(file, at, nullptr, FILE_BEGIN);
    SetEndOfFile(file);
    CloseHandle(file);
}

std::string Narrow(const std::wstring& w, UINT codePage = CP_UTF8) // UTF-8, as the game passes paths
{
    int n = WideCharToMultiByte(codePage, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 1, '\0');
    WideCharToMultiByte(codePage, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    s.resize(n > 0 ? n - 1 : 0);
    return s;
}

// The ANSI code page's bytes for w; false if it can't spell it (or is UTF-8 already).
bool NarrowAnsi(const std::wstring& w, std::string* out)
{
    BOOL lossy = FALSE;
    if (GetACP() == CP_UTF8 ||
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.c_str(), -1, nullptr, 0, nullptr, &lossy) <= 0 || lossy) {
        return false;
    }
    *out = Narrow(w, CP_ACP);
    return true;
}

// "C:\...\replays/x.rpl", as the game passes them
std::string MixedSlashes(const std::wstring& dir, const char* name)
{
    return Narrow(dir) + "replays/" + name;
}

FILETIME FromNow(uint64_t ticks)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER at;
    at.LowPart = now.dwLowDateTime;
    at.HighPart = now.dwHighDateTime;
    at.QuadPart += ticks;
    return {at.LowPart, at.HighPart};
}

// what another plugin of old appended on its own: 0xFFFF, 10 bytes, a footer with another magic
void AppendForeignTrailer(const std::wstring& path)
{
    std::string bytes = "\xFF\xFF";
    bytes.append(10, '\x01');
    uint32_t footer[2] = {10, 1};
    bytes.append(reinterpret_cast<const char*>(footer), sizeof(footer));
    bytes += "DK2MLVL1";

    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

void TestRegistration()
{
    DK2ML_ReplayData a = Data("test.a", SaveA, LoadA);
    Replays_SetOpen(false);
    Expect("AddReplayData before init: refused", Replays_Add(kA, &a) == DK2ML_ERROR && !Replays_Wanted());

    Replays_SetOpen(true);
    DK2ML_ReplayData small = a;
    small.structSize = sizeof(DK2ML_ReplayData) - 1;
    DK2ML_ReplayData spaced = Data("has space", SaveA, LoadA);
    std::string longKey(64, 'k');
    DK2ML_ReplayData tooLong = Data(longKey.c_str(), SaveA, LoadA);
    DK2ML_ReplayData noKey = Data("", SaveA, LoadA);
    DK2ML_ReplayData noLoad = Data("test.noload", SaveA, nullptr);
    Expect("refused: small structSize, bad keys, no callback",
           Replays_Add(kA, nullptr) == DK2ML_ERROR && Replays_Add(kA, &small) == DK2ML_ERROR &&
               Replays_Add(kA, &spaced) == DK2ML_ERROR && Replays_Add(kA, &tooLong) == DK2ML_ERROR &&
               Replays_Add(kA, &noKey) == DK2ML_ERROR && Replays_Add(kA, &noLoad) == DK2ML_ERROR);

    DK2ML_ReplayData b = Data("test.b", SaveNothing, LoadB);
    DK2ML_ReplayData c = Data("test.c", SaveCrash, LoadNothing);
    DK2ML_ReplayData d = Data("test.d", SaveTooMuch, LoadNothing);
    DK2ML_ReplayData e = Data("test.e", SaveNothing, LoadCrash);
    bool added = Replays_Add(kA, &a) == DK2ML_OK && Replays_Add(kB, &b) == DK2ML_OK &&
                 Replays_Add(kC, &c) == DK2ML_OK && Replays_Add(kD, &d) == DK2ML_OK && Replays_Add(kE, &e) == DK2ML_OK;
    DK2ML_ReplayData taken = Data("test.a", SaveNothing, LoadNothing);
    Expect("five keys added; a taken key refused", added && Replays_Add(kB, &taken) == DK2ML_ERROR);
    Replays_SetOpen(false);

    Expect("ReplayWrite outside a save: refused", Replays_Write(kA, nullptr, "x", 1) == DK2ML_ERROR);
}

void TestBlock(const std::wstring& dir)
{
    std::wstring path = dir + L"block.rpl";
    WriteFakeReplay(path);
    uint64_t gameSize = SizeOf(path);
    std::vector<ReplaySection> read;
    Expect("a replay without a block: none", !Replays_ReadBlock(path, &read) && read.empty());

    std::vector<ReplaySection> sections = {{"one", {1, 2, 3}}, {"two", {}}, {"three", std::vector<uint8_t>(1000, 7)}};
    bool appended = Replays_AppendBlock(path, sections);
    bool same = Replays_ReadBlock(path, &read) && read.size() == 3 && read[0].key == "one" &&
                read[0].data == sections[0].data && read[1].data.empty() && read[2].data == sections[2].data;
    Expect("round trip, several sections", appended && same);

    std::wstring renamed = dir + L"saved under a name.rpl";
    DeleteFileW(renamed.c_str());
    MoveFileW(path.c_str(), renamed.c_str());
    Expect("still read after a rename", Replays_ReadBlock(renamed, &read) && read.size() == 3);

    uint64_t full = SizeOf(renamed);
    Truncate(renamed, full - 3);
    Expect("cut footer: none", !Replays_ReadBlock(renamed, &read) && read.empty());

    Truncate(renamed, gameSize);
    Replays_AppendBlock(renamed, sections);
    HANDLE file = CreateFileW(renamed.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(gameSize + 2); // after 0xFFFF: the section count
    SetFilePointerEx(file, at, nullptr, FILE_BEGIN);
    DWORD written = 0;
    uint8_t count = 4; // one more than there are: runs past the payload's end
    WriteFile(file, &count, 1, &written, nullptr);
    CloseHandle(file);
    Expect("damaged payload: none", !Replays_ReadBlock(renamed, &read));

    Truncate(renamed, gameSize);
    std::vector<ReplaySection> badKey = {{std::string(64, 'k'), {1}}};
    std::vector<ReplaySection> tooBig = {{"big", std::vector<uint8_t>(DK2ML_REPLAY_MAX_DATA + 1)}};
    bool refused = !Replays_AppendBlock(renamed, badKey) && !Replays_AppendBlock(renamed, tooBig);
    Expect("a key over 63 characters or data over the cap: not written", refused && SizeOf(renamed) == gameSize);
    Expect("a missing file: none", !Replays_ReadBlock(dir + L"missing.rpl", &read));
}

// Starts around StopRecording: the file the start plays may not exist yet, may be another one, may not take the block.
// A plays g_alpha.
void TestWatching(const std::wstring& dir)
{
    std::wstring replays = dir + L"replays\\";
    std::vector<ReplaySection> read;

    // watching right after a mission, the file written only inside the start; the path with mixed slashes
    std::wstring sixth = replays + L"last_replay_6.rpl";
    std::string sixthPath = MixedSlashes(dir, "last_replay_6.rpl");
    g_alpha = "sixth";
    int loads = g_loadsA;
    Replays_OnReplayStartPre(dir, sixthPath.c_str());
    bool noneYet = g_loadsA == loads;
    Replays_OnStopRecordingPre(dir);
    WriteFakeReplay(sixth);
    Replays_OnStopRecordingPost(dir);
    Replays_OnReplayStartPost();
    Expect("file new inside the start: saved, then loaded once, no NULL first",
           noneYet && g_loadsA == loads + 1 && g_loadedA == "sixth" && Replays_ReadBlock(sixth, &read));
    MakeOld(sixth);

    Replays_OnReplayStartPre(dir, MixedSlashes(dir, "Some Map.rpl").c_str());
    Replays_OnReplayStartPost();
    Expect("a full path with mixed slashes: found", g_loadedA == "alpha");

    // another replay starts while a mission still records: the mission's data is saved, not loaded
    g_alpha = "mission";
    Replays_OnReplayStartPre(dir, "foreign.rpl");
    Replays_OnStopRecordingPre(dir);
    std::wstring seventh = replays + L"last_replay_7.rpl";
    WriteFakeReplay(seventh);
    Replays_OnStopRecordingPost(dir);
    Replays_OnReplayStartPost();
    bool savedMission = Replays_ReadBlock(seventh, &read) && read.size() == 1 &&
                        std::string(read[0].data.begin(), read[0].data.end()) == "mission";
    Expect("another replay starting: its data, the mission's only saved", g_loadedA == "z" && savedMission);
    MakeOld(seventh);

    // a name beyond ASCII, as UTF-8 and in the ANSI code page
    std::wstring mantra = L"Act 1_ “Mantra”.rpl";
    WriteFakeReplay(replays + mantra);
    Replays_AppendBlock(replays + mantra, {{"test.a", {'q'}}});
    MakeOld(replays + mantra);
    g_loadedA.clear();
    Replays_OnReplayStartPre(dir, Narrow(mantra).c_str());
    Replays_OnReplayStartPost();
    Expect("a non-ASCII name in UTF-8: found", g_loadedA == "q");
    std::string ansi;
    if (NarrowAnsi(mantra, &ansi)) {
        g_loadedA.clear();
        Replays_OnReplayStartPre(dir, ansi.c_str());
        Replays_OnReplayStartPost();
        Expect("a non-ASCII name in the ANSI code page: found", g_loadedA == "q");
    } else {
        printf("%-58s %s\n", "a non-ASCII name in the ANSI code page", "skipped (this code page can't spell it)");
    }

    // another plugin's own trailer at the end already: ours goes after it, and is read
    g_alpha = "after";
    std::wstring eighth = replays + L"last_replay_8.rpl";
    Replays_OnStopRecordingPre(dir);
    WriteFakeReplay(eighth);
    AppendForeignTrailer(eighth);
    Replays_OnStopRecordingPost(dir);
    Expect("after a foreign trailer: our block written and read",
           Replays_ReadBlock(eighth, &read) && read.size() == 1 && read[0].key == "test.a");
    MakeOld(eighth);

    // watching right after a mission, the block can't be written: load what the file has (none), as a rewind will
    g_alpha = "lost";
    std::wstring ninth = replays + L"last_replay_9.rpl";
    Replays_OnReplayStartPre(dir, "last_replay_9.rpl");
    Replays_OnStopRecordingPre(dir);
    WriteFakeReplay(ninth);
    SetFileAttributesW(ninth.c_str(), FILE_ATTRIBUTE_READONLY);
    Replays_OnStopRecordingPost(dir);
    Replays_OnReplayStartPost();
    Expect("block not written: A gets NULL, as on a rewind", g_loadedANull && !Replays_ReadBlock(ninth, &read));
    SetFileAttributesW(ninth.c_str(), FILE_ATTRIBUTE_NORMAL);
    MakeOld(ninth);
}

void TestSaveAndLoad(const std::wstring& dir)
{
    std::wstring replays = dir + L"replays\\";
    std::wstring older = replays + L"last_replay_0.rpl";
    std::wstring fresh = replays + L"last_replay_1.rpl";
    WriteFakeReplay(older);
    MakeOld(older);
    DeleteFileW(fresh.c_str());

    // nothing recording: no file is newer, nothing is written
    Replays_OnStopRecordingPre(dir);
    Replays_OnStopRecordingPost(dir);
    std::vector<ReplaySection> read;
    Expect("StopRecording that wrote nothing: no block anywhere", !Replays_ReadBlock(older, &read));

    // a mission ends
    Replays_OnStopRecordingPre(dir);
    WriteFakeReplay(fresh);
    Replays_OnStopRecordingPost(dir);
    bool onlyA = Replays_ReadBlock(fresh, &read) && read.size() == 1 && read[0].key == "test.a" &&
                 std::string(read[0].data.begin(), read[0].data.end()) == "alpha";
    Expect("saved into the file just written: A's data only", onlyA && !Replays_ReadBlock(older, &read));
    Expect("C's crash in save contained and reported", g_crashed == kC && g_crashes == 1);
    Expect("D over the cap: refused, nothing of it stored; NULL data refused",
           g_overflowResult == DK2ML_ERROR && g_nullDataResult == DK2ML_ERROR);
    Expect("not watching: no load ran", g_loadsA == 0 && g_loadsB == 0);

    // StopRecording again on the same file (e.g. a second call): the block isn't doubled
    uint64_t size = SizeOf(fresh);
    Replays_OnStopRecordingPre(dir);
    Replays_OnStopRecordingPost(dir);
    Expect("no second block", SizeOf(fresh) == size);

    // the replays menu: the path under replays\, then a full path
    Replays_OnReplayStartPre(dir, "last_replay_1.rpl");
    Replays_OnReplayStartPost();
    Expect("start by name: A gets its bytes, B gets NULL/0",
           g_loadsA == 1 && g_loadedA == "alpha" && g_loadsB == 1 && g_loadedBNull);
    Expect("E's crash in load contained, the others still loaded", g_crashed == kE && g_crashes == 2);

    std::wstring renamed = replays + L"Some Map.rpl";
    DeleteFileW(renamed.c_str());
    MoveFileW(fresh.c_str(), renamed.c_str());
    Replays_OnReplayStartPre(dir, Narrow(renamed).c_str());
    Replays_OnReplayStartPost();
    Expect("saved under a name, full path: A gets its bytes", g_loadsA == 2 && g_loadedA == "alpha");

    // no data: load(NULL) waits for the start's post (a StopRecording inside the start would save first)
    Replays_OnReplayStartPre(dir, "nothing here.rpl");
    bool notYet = g_loadsA == 2;
    Replays_OnReplayStartPost();
    Expect("a file that isn't there: A gets NULL/0 once, in the post", notYet && g_loadsA == 3 && g_loadedANull);

    Replays_OnReplayStartPre(dir, "last_replay_0.rpl");
    Replays_OnReplayStartPost();
    Expect("an older replay without a block: A gets NULL/0", g_loadsA == 4 && g_loadedANull);

    // a block with a key no plugin has: ignored
    std::wstring foreign = replays + L"foreign.rpl";
    WriteFakeReplay(foreign);
    Replays_AppendBlock(foreign, {{"someone.else", {9, 9}}, {"test.a", {'z'}}});
    Replays_OnReplayStartPre(dir, "foreign.rpl");
    Replays_OnReplayStartPost();
    Expect("unknown keys ignored, A still gets its bytes", g_loadsA == 5 && g_loadedA == "z");

    // watching right after a mission: the start reads a stale last_replay_2 (an old block), then StopRecording inside
    // it writes the file anew
    std::wstring stale = replays + L"last_replay_2.rpl";
    WriteFakeReplay(stale);
    Replays_AppendBlock(stale, {{"test.a", {'o', 'l', 'd'}}});
    MakeOld(stale);
    MakeOld(renamed);
    MakeOld(foreign);
    g_alpha = "fresh";
    Replays_OnReplayStartPre(dir, "last_replay_2.rpl");
    bool sawOld = g_loadedA == "old";
    Replays_OnStopRecordingPre(dir);
    WriteFakeReplay(stale);
    Replays_OnStopRecordingPost(dir);
    Replays_OnReplayStartPost();
    Expect("nested StopRecording: saved, then loaded the fresh data",
           sawOld && g_loadedA == "fresh" && Replays_ReadBlock(stale, &read) && read.size() == 1);

    // after the start finished, a StopRecording doesn't load
    int loads = g_loadsA;
    MakeOld(stale);
    Replays_OnStopRecordingPre(dir);
    WriteFakeReplay(fresh);
    Replays_OnStopRecordingPost(dir);
    Expect("StopRecording outside a start: saved, no load", g_loadsA == loads && Replays_ReadBlock(fresh, &read));

    // the game writes the file all mission: its last packet and the header can get the same write time and size, so
    // the file isn't changed since before the call, only written during it (a second from now stands in for that)
    MakeOld(fresh);
    std::wstring sameTick = replays + L"last_replay_4.rpl";
    WriteFakeReplay(sameTick);
    FILETIME tick = FromNow(10000000ull); // a second
    SetWriteTime(sameTick, tick);
    Replays_OnStopRecordingPre(dir);
    WriteFakeReplay(sameTick); // the header
    SetWriteTime(sameTick, tick);
    Replays_OnStopRecordingPost(dir);
    Expect("written in the same tick as before the call: still saved", Replays_ReadBlock(sameTick, &read));
    MakeOld(sameTick);

    // a file dated in the future, untouched: a StopRecording that wrote nothing doesn't take it for the new one
    std::wstring future = replays + L"last_replay_5.rpl";
    WriteFakeReplay(future);
    SetWriteTime(future, FromNow(36000000000ull)); // an hour
    Replays_OnStopRecordingPre(dir);
    Replays_OnStopRecordingPost(dir);
    Expect("an untouched file dated in the future: no block", !Replays_ReadBlock(future, &read));
    DeleteFileW(future.c_str());

    TestWatching(dir);

    // a switched-off plugin: neither save nor load
    loads = g_loadsA;
    Replays_FaultOwner(kA);
    MakeOld(fresh);
    std::wstring next = replays + L"last_replay_3.rpl";
    Replays_OnStopRecordingPre(dir);
    WriteFakeReplay(next);
    Replays_OnStopRecordingPost(dir);
    Replays_OnReplayStartPre(dir, "last_replay_1.rpl");
    Replays_OnReplayStartPost();
    Expect("switched off: no save (no block), no load", !Replays_ReadBlock(next, &read) && g_loadsA == loads);
}

// Last: the loader's own crash here (a path argument that isn't one) turns replay data off, and nothing runs after.
void TestLoaderCrash(const std::wstring& dir)
{
    int loads = g_loadsB;
    Replays_OnReplayStartPre(dir, reinterpret_cast<const char*>(1));
    Replays_OnReplayStartPost();
    bool contained = g_loadsB == loads; // the crash came before anything was owed

    Replays_OnReplayStartPre(dir, "nothing here.rpl");
    Replays_OnReplayStartPost();
    Expect("the loader's crash contained; replay data off after it", contained && g_loadsB == loads);
}

} // namespace

int main()
{
    LogOpen(L"replaystest.log");
    Replays_SetCrashCallback(OnCrash);
    std::wstring dir = TempDir();

    TestRegistration();
    TestBlock(dir);
    TestSaveAndLoad(dir);
    TestLoaderCrash(dir);

    printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
