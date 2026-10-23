// Replay data (DK2ML_API::AddReplayData): plugins' bytes inside the game's replay files, one block for all of them.
//
// A replay (.rpl) is a 0xF8 byte header, then [u16 length][packet] chunks; the game reads chunks until a length of 0 or
// over 0x74B3. So a block starting with 0xFFFF after the last chunk is never read by it, and goes along when the player
// saves the replay (a rename). Block: u16 0xFFFF, the payload, then a 16-byte footer found from the file's end.
// Payload, little-endian: u32 section count, per section u8 key length, the key, u32 size, the bytes.
//
// GameHooks.cpp calls the Replays_On* functions from Replay::StopRecording (writes last_replay_N.rpl) and
// GameClient::RequestReplayStart (plays a file). Watching right after a mission, StopRecording runs inside the start:
// the start's pre finds no block yet, so load(NULL) waits for its post, and the nested StopRecording saves and then
// loads what it wrote, read back from the file as a rewind would.
// Registrations are taken only during init, so the main thread reads them without a lock. Callbacks run under __try; a
// crash switches off that plugin. A crash in the loader's own code here turns replay data off, nothing else. Touches
// files, not the game (replaystest).
#include "Loader.h"

#include <malloc.h> // _resetstkoflw

#include <cstring>

struct DK2ML_ReplayWriter {
    const char* key;
    std::vector<uint8_t> bytes;
    bool overflow; // past DK2ML_REPLAY_MAX_DATA: logged, nothing stored
};

namespace {

struct Callbacks {
    void (*save)(DK2ML_ReplayWriter* out, void* user);
    void (*load)(const void* data, size_t size, void* user);
    void* user;
};

struct Registration {
    std::string key;
    Callbacks fns;
    HMODULE owner;
    volatile bool faulted; // owner switched off
};

// a RequestReplayStart in progress
struct Start {
    std::string path; // as the game gave it
    bool pending; // no data loaded yet: load(NULL) in its post
};

// last_replay_*.rpl before StopRecording
struct Stamp {
    std::wstring path;
    uint64_t written;
    uint64_t size;
};

std::vector<Registration> g_registrations;
bool g_open = false;
void (*g_onCrash)(HMODULE owner) = nullptr;

DK2ML_ReplayWriter* g_writer = nullptr; // during a save callback
std::vector<Stamp> g_before; // StopRecording's pre: the last replays then
uint64_t g_stopStarted = 0; // ... and the time then
std::vector<Start> g_starts; // nested calls stack
bool g_toldWriteFailure = false;
bool g_off = false; // the loader crashed in here: replay data is off for the session

constexpr uint16_t kStopLength = 0xFFFF;
constexpr char kMagic[8] = {'D', 'K', '2', 'M', 'L', 'R', 'P', '1'};
constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kMaxPayload = 4u << 20; // all sections
constexpr size_t kMaxReplayKey = 63; // the file format's; keys in older files must stay readable
constexpr uint64_t kClockSlack = 3 * 10000000ull; // 3 s in FILETIME ticks: a FAT disk rounds write times up to 2 s

struct Footer {
    uint32_t payloadSize;
    uint32_t version;
    char magic[8];
};

static_assert(sizeof(Footer) == 16, "the footer is 16 bytes on disk");

thread_local DWORD t_code;

int Filter(DWORD code)
{
    t_code = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

// No destructors in here (__try).
bool CallSave(const Callbacks& fns, DK2ML_ReplayWriter* out)
{
    __try {
        fns.save(out, fns.user);
        return true;
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

bool CallLoad(const Callbacks& fns, const void* data, size_t size)
{
    __try {
        fns.load(data, size, fns.user);
        return true;
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

void ResetStackIfOverflowed()
{
    if (t_code == EXCEPTION_STACK_OVERFLOW) {
        _resetstkoflw();
    }
}

void Contain(HMODULE owner, const char* where)
{
    ResetStackIfOverflowed();
    LogF("%ls crashed in its %s (exception 0x%08lX)", Log_ModuleName(owner).c_str(), where, t_code);
    Replays_FaultOwner(owner);
    if (g_onCrash) {
        g_onCrash(owner); // its hooks, events and options too
    }
}

// The loader's own code here crashed (e.g. a path argument that isn't one): the game and the loader's other hooks go on.
void TurnOff(const char* where)
{
    ResetStackIfOverflowed();
    g_off = true;
    g_writer = nullptr;
    LogF("replay data: the loader crashed in %s (exception 0x%08lX): plugins' replay data is off for this session",
         where, t_code);
}

const Registration* Find(const std::string& key)
{
    for (const auto& r : g_registrations) {
        if (r.key == key) {
            return &r;
        }
    }
    return nullptr;
}

const ReplaySection* FindSection(const std::vector<ReplaySection>& sections, const std::string& key)
{
    for (const auto& s : sections) {
        if (s.key == key) {
            return &s;
        }
    }
    return nullptr;
}

struct File {
    HANDLE handle;

    explicit File(HANDLE h) : handle(h) {}

    ~File()
    {
        if (Ok()) {
            CloseHandle(handle);
        }
    }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool Ok() const { return handle != INVALID_HANDLE_VALUE; }

    bool ReadAt(LONGLONG offset, void* out, DWORD size) const
    {
        LARGE_INTEGER at;
        at.QuadPart = offset;
        DWORD read = 0;
        return SetFilePointerEx(handle, at, nullptr, FILE_BEGIN) && ReadFile(handle, out, size, &read, nullptr) &&
               read == size;
    }
};

template <typename T> void Put(std::vector<uint8_t>& out, T value)
{
    uint8_t bytes[sizeof(T)];
    std::memcpy(bytes, &value, sizeof(T));
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <typename T> bool Get(const uint8_t*& at, const uint8_t* end, T* value)
{
    if (static_cast<size_t>(end - at) < sizeof(T)) {
        return false;
    }
    std::memcpy(value, at, sizeof(T));
    at += sizeof(T);
    return true;
}

bool Decode(const std::vector<uint8_t>& payload, std::vector<ReplaySection>* sections)
{
    const uint8_t* at = payload.data();
    const uint8_t* end = at + payload.size();
    uint32_t count = 0;
    if (!Get(at, end, &count)) {
        return false;
    }

    std::vector<ReplaySection> decoded;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t keyLength = 0;
        if (!Get(at, end, &keyLength) || keyLength == 0 || keyLength > kMaxReplayKey ||
            static_cast<size_t>(end - at) < keyLength) {
            return false;
        }
        std::string key(reinterpret_cast<const char*>(at), keyLength);
        at += keyLength;

        uint32_t size = 0;
        if (!Get(at, end, &size) || size > DK2ML_REPLAY_MAX_DATA || static_cast<size_t>(end - at) < size) {
            return false;
        }
        decoded.push_back({key, std::vector<uint8_t>(at, at + size)});
        at += size;
    }

    if (at != end) {
        return false;
    }
    *sections = std::move(decoded);
    return true;
}

uint64_t Ticks(const FILETIME& time)
{
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

uint64_t Now()
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return Ticks(now);
}

// Every last_replay_*.rpl in <userDir>replays\, with the write time and size from the file itself (the directory
// listing's copy can lag behind).
std::vector<Stamp> LastReplays(const std::wstring& userDir)
{
    std::vector<Stamp> stamps;
    if (userDir.empty()) {
        return stamps;
    }

    std::wstring dir = userDir + L"replays\\";
    WIN32_FIND_DATAW found;
    HANDLE find = FindFirstFileW((dir + L"last_replay_*.rpl").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) {
        return stamps;
    }

    do {
        std::wstring path = dir + found.cFileName;
        WIN32_FILE_ATTRIBUTE_DATA data;
        bool isFile = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) &&
                      !(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        if (isFile) {
            uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            stamps.push_back({path, Ticks(data.ftLastWriteTime), size});
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return stamps;
}

// The file StopRecording just wrote: the newest one that's new or changed since its pre. Else the newest one written
// during the call: the game writes the file all mission, so its last packet and the header can share a clock tick (and
// the size). Not one dated in the future. StopRecording is a no-op when nothing records.
bool JustWritten(const std::wstring& userDir, std::wstring* path)
{
    uint64_t now = Now();
    uint64_t newestChanged = 0;
    uint64_t newestDuring = 0;
    std::wstring changed, during;
    for (const Stamp& s : LastReplays(userDir)) {
        const Stamp* before = nullptr;
        for (const Stamp& b : g_before) {
            if (_wcsicmp(b.path.c_str(), s.path.c_str()) == 0) {
                before = &b;
            }
        }

        bool isChanged = !before || before->written != s.written || before->size != s.size;
        if (isChanged && s.written >= newestChanged) {
            newestChanged = s.written;
            changed = s.path;
        }
        bool isDuring = s.written >= g_stopStarted && s.written <= now + kClockSlack;
        if (isDuring && s.written >= newestDuring) {
            newestDuring = s.written;
            during = s.path;
        }
    }

    *path = !changed.empty() ? changed : during;
    return !path->empty();
}

std::wstring Widen(const char* text, UINT codePage)
{
    int count = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);
    if (count <= 1) {
        return L"";
    }
    std::wstring wide(count, L'\0');
    MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, text, -1, wide.data(), count);
    wide.resize(count - 1);
    return wide;
}

// Where the game's replay path may be: as given, under userDir or its replays\ (UTF-8, else the ANSI code page).
std::vector<std::wstring> Candidates(const std::wstring& userDir, const char* path)
{
    std::vector<std::wstring> candidates;
    if (!path || !path[0]) {
        return candidates;
    }

    for (UINT codePage : {static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP)}) {
        std::wstring wide = Widen(path, codePage);
        if (wide.empty()) {
            continue;
        }
        candidates.push_back(wide);
        if (!userDir.empty()) {
            candidates.push_back(userDir + wide);
            candidates.push_back(userDir + L"replays\\" + wide);
        }
    }
    return candidates;
}

std::wstring FullPath(const std::wstring& path)
{
    DWORD count = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (count == 0) {
        return path;
    }
    std::wstring full(count, L'\0');
    DWORD written = GetFullPathNameW(path.c_str(), count, full.data(), nullptr);
    full.resize(written < count ? written : 0);
    return full.empty() ? path : full;
}

// The game mixes slashes ("...\Local/KillHouseGames/...") and doesn't care about case.
bool SamePath(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(FullPath(a).c_str(), FullPath(b).c_str()) == 0;
}

// The innermost start plays this file. It may not have existed at the start's pre, so compare the candidates.
bool IsStarting(const std::wstring& userDir, const std::wstring& file)
{
    if (g_starts.empty()) {
        return false;
    }
    for (const std::wstring& candidate : Candidates(userDir, g_starts.back().path.c_str())) {
        if (SamePath(candidate, file)) {
            return true;
        }
    }
    return false;
}

std::wstring FileName(const std::wstring& path)
{
    return path.substr(path.find_last_of(L"\\/") + 1);
}

// "\"a\" (12 bytes), \"b\" (3 bytes)"
std::string Describe(const std::vector<ReplaySection>& sections)
{
    std::string text;
    for (const auto& s : sections) {
        text += (text.empty() ? "\"" : ", \"") + s.key + "\" (" + std::to_string(s.data.size()) + " bytes)";
    }
    return text;
}

void StopRecordingPre(const std::wstring& userDir)
{
    g_before = LastReplays(userDir);
    g_stopStarted = Now();
}

void StopRecordingPost(const std::wstring& userDir)
{
    std::wstring path;
    if (!JustWritten(userDir, &path)) {
        return;
    }

    std::wstring name = FileName(path);
    std::vector<ReplaySection> sections;
    if (!Replays_ReadBlock(path, &sections)) { // else it already has one
        sections = Replays_Save();
        if (sections.empty()) {
            // nothing to store
        } else if (Replays_AppendBlock(path, sections)) {
            LogF("replay: saved %s into %ls", Describe(sections).c_str(), name.c_str());
        } else if (!g_toldWriteFailure) {
            g_toldWriteFailure = true;
            LogF("replay: couldn't write plugins' replay data into %ls", name.c_str());
        }
    }

    // watching it now: the start read the file before this wrote it. What's in the file, as a rewind will read it.
    if (IsStarting(userDir, path)) {
        Replays_ReadBlock(path, &sections);
        g_starts.back().pending = false;
        Replays_Load(sections);
        if (sections.empty()) {
            LogF("replay: no plugin data in %ls", name.c_str());
        } else {
            LogF("replay: %s from %ls", Describe(sections).c_str(), name.c_str());
        }
    }
}

void ReplayStartPre(const std::wstring& userDir, const char* path)
{
    g_starts.push_back({path ? path : "", true});

    std::wstring file = Replays_Resolve(userDir, path);
    std::vector<ReplaySection> sections;
    if (!file.empty() && Replays_ReadBlock(file, &sections)) {
        g_starts.back().pending = false;
        Replays_Load(sections);
        LogF("replay: %s from %s", Describe(sections).c_str(), g_starts.back().path.c_str());
    }
}

// Without data so far: load(NULL) now, not in the pre, so a StopRecording inside the start saves before any load.
void ReplayStartPost()
{
    if (g_starts.empty()) {
        return;
    }
    Start start = std::move(g_starts.back());
    g_starts.pop_back();

    if (start.pending) {
        Replays_Load({});
        LogF("replay: no plugin data in %s", start.path.c_str());
    }
}

} // namespace

void Replays_SetOpen(bool open)
{
    g_open = open;
}

void Replays_SetCrashCallback(void (*onCrash)(HMODULE owner))
{
    g_onCrash = onCrash;
}

const char* Replays_Check(const DK2ML_ReplayData* data)
{
    if (!data) {
        return "no DK2ML_ReplayData";
    }
    if (data->structSize < sizeof(DK2ML_ReplayData)) {
        return "structSize is smaller than sizeof(DK2ML_ReplayData)";
    }
    if (!data->key || !*data->key) {
        return "no key";
    }

    size_t length = strnlen(data->key, kMaxReplayKey + 1);
    if (length > kMaxReplayKey) {
        return "key longer than 63 characters";
    }
    for (size_t i = 0; i < length; ++i) {
        if (data->key[i] <= ' ' || data->key[i] > '~') {
            return "key has characters other than printable ASCII (no spaces)";
        }
    }

    if (!data->save || !data->load) {
        return "no save or load callback";
    }
    return nullptr;
}

DK2ML_Status Replays_Add(HMODULE owner, const DK2ML_ReplayData* data)
{
    std::wstring who = Log_ModuleName(owner);
    if (!g_open) {
        LogF("%ls: AddReplayData after DK2ML_PluginInit returned, ignored (add it in init)", who.c_str());
        return DK2ML_ERROR;
    }
    if (const char* error = Replays_Check(data)) {
        LogF("%ls: AddReplayData ignored: %s", who.c_str(), error);
        return DK2ML_ERROR;
    }
    if (const Registration* taken = Find(data->key)) {
        LogF("%ls: AddReplayData(\"%s\") ignored: %ls already uses that key", who.c_str(), data->key,
             Log_ModuleName(taken->owner).c_str());
        return DK2ML_ERROR;
    }

    g_registrations.push_back({data->key, {data->save, data->load, data->user}, owner, false});
    return DK2ML_OK;
}

bool Replays_Wanted()
{
    for (const auto& r : g_registrations) {
        if (!r.faulted) {
            return true;
        }
    }
    return false;
}

void Replays_FaultOwner(HMODULE owner)
{
    for (auto& r : g_registrations) {
        if (r.owner == owner) {
            r.faulted = true;
        }
    }
}

void Replays_Log()
{
    for (const auto& r : g_registrations) {
        if (!r.faulted) {
            LogF("replay data \"%s\": %ls", r.key.c_str(), Log_ModuleName(r.owner).c_str());
        }
    }
}

DK2ML_Status Replays_Write(HMODULE caller, DK2ML_ReplayWriter* out, const void* data, size_t size)
{
    if (!out || out != g_writer) {
        LogF("%ls: ReplayWrite outside its replay data save callback, ignored", Log_ModuleName(caller).c_str());
        return DK2ML_ERROR;
    }
    if (out->overflow) {
        return DK2ML_ERROR; // already logged
    }
    if (!data && size) {
        LogF("%ls: ReplayWrite(\"%s\") with NULL data, ignored", Log_ModuleName(caller).c_str(), out->key);
        return DK2ML_ERROR;
    }
    if (size > DK2ML_REPLAY_MAX_DATA - out->bytes.size()) {
        out->overflow = true;
        LogF("%ls: replay data \"%s\" is over %u bytes: none of it is stored in this replay",
             Log_ModuleName(caller).c_str(), out->key, DK2ML_REPLAY_MAX_DATA);
        return DK2ML_ERROR;
    }

    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    out->bytes.insert(out->bytes.end(), bytes, bytes + size);
    return DK2ML_OK;
}

bool Replays_ReadBlock(const std::wstring& path, std::vector<ReplaySection>* sections)
{
    sections->clear();
    File file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    LARGE_INTEGER size;
    Footer footer;
    bool haveFooter = file.Ok() && GetFileSizeEx(file.handle, &size) &&
                      size.QuadPart >= static_cast<LONGLONG>(sizeof(Footer) + sizeof(kStopLength)) &&
                      file.ReadAt(size.QuadPart - sizeof(Footer), &footer, sizeof(Footer));
    bool ours = haveFooter && std::memcmp(footer.magic, kMagic, sizeof(kMagic)) == 0 &&
                footer.version == kFormatVersion && footer.payloadSize <= kMaxPayload &&
                size.QuadPart >= static_cast<LONGLONG>(sizeof(Footer) + sizeof(kStopLength) + footer.payloadSize);
    if (!ours) {
        return false;
    }

    LONGLONG start = size.QuadPart - sizeof(Footer) - footer.payloadSize - sizeof(kStopLength);
    uint16_t stop = 0;
    std::vector<uint8_t> payload(footer.payloadSize);
    bool read = file.ReadAt(start, &stop, sizeof(stop)) && stop == kStopLength &&
                (payload.empty() || file.ReadAt(start + sizeof(stop), payload.data(), footer.payloadSize));
    return read && Decode(payload, sections);
}

bool Replays_AppendBlock(const std::wstring& path, const std::vector<ReplaySection>& sections)
{
    std::vector<uint8_t> payload;
    Put<uint32_t>(payload, static_cast<uint32_t>(sections.size()));
    for (const auto& s : sections) {
        bool keyOk = !s.key.empty() && s.key.size() <= kMaxReplayKey;
        if (!keyOk || s.data.size() > DK2ML_REPLAY_MAX_DATA) {
            return false;
        }
        Put<uint8_t>(payload, static_cast<uint8_t>(s.key.size()));
        payload.insert(payload.end(), s.key.begin(), s.key.end());
        Put<uint32_t>(payload, static_cast<uint32_t>(s.data.size()));
        payload.insert(payload.end(), s.data.begin(), s.data.end());
    }
    if (payload.size() > kMaxPayload) {
        return false;
    }

    std::vector<uint8_t> block;
    Put<uint16_t>(block, kStopLength);
    block.insert(block.end(), payload.begin(), payload.end());
    Footer footer{static_cast<uint32_t>(payload.size()), kFormatVersion, {}};
    std::memcpy(footer.magic, kMagic, sizeof(kMagic));
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&footer);
    block.insert(block.end(), bytes, bytes + sizeof(Footer));

    File file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr));
    DWORD written = 0;
    return file.Ok() && WriteFile(file.handle, block.data(), static_cast<DWORD>(block.size()), &written, nullptr) &&
           written == block.size();
}

std::wstring Replays_Resolve(const std::wstring& userDir, const char* path)
{
    for (const std::wstring& candidate : Candidates(userDir, path)) {
        DWORD attributes = GetFileAttributesW(candidate.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return candidate;
        }
    }
    return L"";
}

std::vector<ReplaySection> Replays_Save()
{
    std::vector<ReplaySection> sections;
    size_t total = 0;
    for (const auto& r : g_registrations) {
        if (r.faulted) {
            continue;
        }

        DK2ML_ReplayWriter writer{r.key.c_str(), {}, false};
        g_writer = &writer;
        bool ok = CallSave(r.fns, &writer);
        g_writer = nullptr;
        if (!ok) {
            Contain(r.owner, "replay data save callback");
            continue;
        }

        if (writer.overflow || writer.bytes.empty()) {
            continue;
        }
        size_t room = kMaxPayload - total;
        size_t needed = 1 + r.key.size() + 4 + writer.bytes.size() + (sections.empty() ? 4 : 0);
        if (needed > room) {
            LogF("%ls: replay data \"%s\" not stored: all plugins' data together is over %u bytes",
                 Log_ModuleName(r.owner).c_str(), r.key.c_str(), kMaxPayload);
            continue;
        }
        total += needed;
        sections.push_back({r.key, std::move(writer.bytes)});
    }
    return sections;
}

void Replays_Load(const std::vector<ReplaySection>& sections)
{
    for (const auto& r : g_registrations) {
        if (r.faulted) {
            continue;
        }

        const ReplaySection* s = FindSection(sections, r.key);
        const void* data = s && !s->data.empty() ? s->data.data() : nullptr;
        size_t size = data ? s->data.size() : 0;
        if (!CallLoad(r.fns, data, size)) {
            Contain(r.owner, "replay data load callback");
        }
    }
}

// The hooks' entries. No destructors in here (__try).
void Replays_OnStopRecordingPre(const std::wstring& userDir)
{
    if (g_off) {
        return;
    }
    __try {
        StopRecordingPre(userDir);
    } __except (Filter(GetExceptionCode())) {
        TurnOff("Replay::StopRecording");
    }
}

void Replays_OnStopRecordingPost(const std::wstring& userDir)
{
    if (g_off) {
        return;
    }
    __try {
        StopRecordingPost(userDir);
    } __except (Filter(GetExceptionCode())) {
        TurnOff("Replay::StopRecording");
    }
}

void Replays_OnReplayStartPre(const std::wstring& userDir, const char* path)
{
    if (g_off) {
        return;
    }
    __try {
        ReplayStartPre(userDir, path);
    } __except (Filter(GetExceptionCode())) {
        TurnOff("GameClient::RequestReplayStart");
    }
}

void Replays_OnReplayStartPost()
{
    if (g_off) {
        return;
    }
    __try {
        ReplayStartPost();
    } __except (Filter(GetExceptionCode())) {
        TurnOff("GameClient::RequestReplayStart");
    }
}
