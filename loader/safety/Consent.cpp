// Which mod folders may run code, and the player's permission for Workshop code.
//
// - Classification: local folders (<save>\mods\, <save>\mods_upload\, <game>\mods_native\) load without asking.
//   Steam Workshop items need the player's permission, and anything else is skipped. Paths are canonicalized first
//   (junctions, symlinks, "..").
// - Consent: Workshop items update by themselves, so "I trust this mod" can only mean "I trust this version". The
//   answer is stored per item with a fingerprint of its whole native\ folder (dk2ml.ini [workshop]
//   <itemId>=allow:<sha256> or deny:<sha256>), and a different fingerprint asks again. One prompt at startup lists
//   every item that's new or changed.
//
// This is a heads-up for players, not a security boundary: native code has full access to the PC.
#include "Loader.h"

#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

constexpr wchar_t kSection[] = L"workshop";
constexpr wchar_t kWorkshopContent[] = L"\\steamapps\\workshop\\content\\1239080\\"; // Door Kickers 2's app id
constexpr size_t kMaxWorkshopIdDigits = 20; // a Workshop item id is a 64-bit number

constexpr uint64_t kMaxNativeBytes = 256ull << 20; // refuse to fingerprint (and so load) more than this
constexpr size_t kMaxModXmlBytes = 64 << 10; // a title is at the top of mod.xml
constexpr size_t kMaxTitleBytes = 100; // mod.xml titles are cut here (UTF-8 bytes, after decoding entities)
constexpr size_t kSha256Bytes = 32;

constexpr LONG kMaxNtHeaderOffset = 4096; // the NT headers follow the DOS stub; further out is malformed
constexpr size_t kMaxEntitySpan = 10; // '&' to ';': the longest valid one is &#x10FFFF;

std::wstring Lower(std::wstring s)
{
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring WithSlash(std::wstring p)
{
    if (!p.empty() && p.back() != L'\\') {
        p += L'\\';
    }
    return p;
}

bool ReadFileBytes(const std::wstring& path, std::string* out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    *out = ss.str();
    return !file.bad();
}

class Sha256 {
  public:
    Sha256()
    {
        if (BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
            m_ok = BCryptCreateHash(m_alg, &m_hash, nullptr, 0, nullptr, 0, 0) == 0;
        }
    }

    ~Sha256()
    {
        if (m_hash) {
            BCryptDestroyHash(m_hash);
        }
        if (m_alg) {
            BCryptCloseAlgorithmProvider(m_alg, 0);
        }
    }

    void Add(const void* data, size_t size)
    {
        if (m_ok && size) {
            m_ok =
                BCryptHashData(m_hash, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), 0) == 0;
        }
    }

    bool Finish(unsigned char (&digest)[kSha256Bytes])
    {
        return m_ok && BCryptFinishHash(m_hash, digest, sizeof(digest), 0) == 0;
    }

  private:
    BCRYPT_ALG_HANDLE m_alg = nullptr;
    BCRYPT_HASH_HANDLE m_hash = nullptr;
    bool m_ok = false;
};

std::wstring Hex(const unsigned char* bytes, size_t n)
{
    static const wchar_t digits[] = L"0123456789abcdef";
    std::wstring s;
    for (size_t i = 0; i < n; ++i) {
        s += digits[bytes[i] >> 4];
        s += digits[bytes[i] & 15];
    }
    return s;
}

// At most maxBytes from the start of the file.
bool ReadFileHead(const std::wstring& path, size_t maxBytes, std::string* out)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    out->resize(maxBytes);
    file.read(out->data(), static_cast<std::streamsize>(maxBytes));
    out->resize(static_cast<size_t>(file.gcount()));
    return !file.bad();
}

bool IsNameChar(char c)
{
    return isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == ':' || c == '.';
}

// The first name="value" in text (any element), as the game's own files write it.
bool FindAttribute(const std::string& text, const char* name, std::string* value)
{
    size_t len = strlen(name);
    for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
        if (at > 0 && IsNameChar(text[at - 1])) {
            continue;
        }
        size_t i = at + len;
        while (i < text.size() && isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i >= text.size() || text[i] != '=') {
            continue;
        }
        ++i;
        while (i < text.size() && isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i >= text.size() || text[i] != '"') {
            continue;
        }
        size_t end = text.find('"', i + 1);
        if (end == std::string::npos) {
            return false;
        }
        *value = text.substr(i + 1, end - i - 1);
        return true;
    }
    return false;
}

std::wstring StoredDecision(const std::wstring& ini, const std::wstring& id)
{
    wchar_t value[256] = {};
    GetPrivateProfileStringW(kSection, id.c_str(), L"", value, 256, ini.c_str());
    return value;
}

bool ListRecursive(const std::wstring& root, const std::wstring& rel, std::vector<NativeFile>* out, uint64_t* total)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((root + rel + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    bool ok = true;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            // a link inside the folder could point anywhere; nothing legitimate needs one
            LogF("%ls%ls%ls is a link, refusing the folder", root.c_str(), rel.c_str(), name.c_str());
            ok = false;
        } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = ListRecursive(root, rel + name + L"\\", out, total);
        } else {
            uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            *total += size;
            out->push_back({rel + name, root + rel + name, size});
        }
    } while (ok && *total <= kMaxNativeBytes && FindNextFileW(h, &fd));
    FindClose(h);

    if (*total > kMaxNativeBytes) {
        LogF("%ls holds more than %llu MB, refusing it", root.c_str(), kMaxNativeBytes >> 20);
        return false;
    }
    return ok;
}

// The helpers below read an untrusted mapped image; they're only called inside ReadPluginExports' __try.

// [rva, rva + bytes) lies inside an image of `size` bytes
bool InImage(DWORD rva, DWORD size, size_t bytes)
{
    return rva < size && size - rva >= bytes;
}

// a table of `count` entries of `entrySize` bytes at rva lies inside the image
bool TableInImage(DWORD rva, DWORD count, size_t entrySize, DWORD size)
{
    return rva < size && count <= (size - rva) / entrySize;
}

// the export name at rva is `name` (N includes the NUL, so a longer name doesn't match)
template <size_t N> bool ExportNameIs(const BYTE* base, DWORD size, DWORD rva, const char (&name)[N])
{
    return InImage(rva, size, N) && memcmp(base + rva, name, N) == 0;
}

bool ValidDosHeader(const IMAGE_DOS_HEADER* dos)
{
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    return dos->e_lfanew > 0 && dos->e_lfanew <= kMaxNtHeaderOffset;
}

// a 64-bit image that has an export directory entry
bool IsAmd64Image(const IMAGE_NT_HEADERS64* nt)
{
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        return false;
    }
    return nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
           nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT;
}

// No destructors in here (__try).
// The image is untrusted (a Workshop item before the player answered), so every RVA is checked against the image size,
// and anything malformed is "not a plugin". manifest gets the DK2ML_PluginManifest export's bytes, up to
// sizeof(DK2ML_Manifest), or structSize 0 if there is none. Its texts are inline character arrays because a file mapped
// as a resource isn't relocated.
bool ReadPluginExports(const BYTE* base, DK2ML_Manifest* manifest)
{
    memset(manifest, 0, sizeof(*manifest));
    __try {
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (!ValidDosHeader(dos)) {
            return false;
        }
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (!IsAmd64Image(nt)) {
            return false;
        }

        DWORD size = nt->OptionalHeader.SizeOfImage;
        DWORD dirRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        if (!dirRva || !InImage(dirRva, size, sizeof(IMAGE_EXPORT_DIRECTORY))) {
            return false;
        }
        auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dirRva);
        DWORD namesRva = exports->AddressOfNames;
        if (!TableInImage(namesRva, exports->NumberOfNames, sizeof(DWORD), size)) {
            return false;
        }

        auto* names = reinterpret_cast<const DWORD*>(base + namesRva);
        bool init = false;
        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            DWORD rva = names[i];
            if (ExportNameIs(base, size, rva, DK2ML_PLUGIN_INIT_NAME)) {
                init = true;
                continue;
            }
            if (!ExportNameIs(base, size, rva, DK2ML_MANIFEST_NAME)) {
                continue;
            }

            // name -> ordinal -> address, every table checked
            DWORD ordinalsRva = exports->AddressOfNameOrdinals;
            DWORD functionsRva = exports->AddressOfFunctions;
            bool tablesInImage = TableInImage(ordinalsRva, exports->NumberOfNames, sizeof(WORD), size) &&
                                 TableInImage(functionsRva, exports->NumberOfFunctions, sizeof(DWORD), size);
            if (!tablesInImage) {
                continue;
            }
            WORD ordinal = reinterpret_cast<const WORD*>(base + ordinalsRva)[i];
            if (ordinal >= exports->NumberOfFunctions) {
                continue;
            }
            DWORD at = reinterpret_cast<const DWORD*>(base + functionsRva)[ordinal];
            DWORD dirSize = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
            if (at >= dirRva && at - dirRva < dirSize) {
                continue; // a forwarder ("other.dll!name"), not data
            }

            constexpr DWORD kHead = offsetof(DK2ML_Manifest, name);
            if (!InImage(at, size, kHead)) {
                continue;
            }
            DWORD declared = *reinterpret_cast<const uint32_t*>(base + at);
            DWORD copy = declared < sizeof(DK2ML_Manifest) ? declared : static_cast<DWORD>(sizeof(DK2ML_Manifest));
            if (copy < kHead || size - at < copy) {
                continue;
            }
            memcpy(manifest, base + at, copy);
        }
        return init;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        memset(manifest, 0, sizeof(*manifest));
        return false;
    }
}

// A manifest text: up to its array's size (a full array has no NUL), control characters as spaces, trimmed.
std::string ManifestText(const char* field, size_t capacity)
{
    std::string s(field, strnlen(field, capacity));
    for (auto& c : s) {
        if (static_cast<unsigned char>(c) < 32 || c == 127) {
            c = ' ';
        }
    }
    size_t first = s.find_first_not_of(' '), last = s.find_last_not_of(' ');
    return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
}

// dir is inside root (both lowercase, with trailing backslashes), not root itself
bool IsStrictlyUnder(const std::wstring& dir, const std::wstring& root)
{
    return !root.empty() && dir.size() > root.size() && dir.compare(0, root.size(), root) == 0;
}

bool IsWorkshopId(const std::wstring& id)
{
    return id.size() <= kMaxWorkshopIdDigits && id.find_first_not_of(L"0123456789") == std::wstring::npos;
}

// Two mods ship different files (or one that couldn't be hashed) under one DLL name.
bool IsClash(const SupportDll& a, const SupportDll& b)
{
    if (a.mod == b.mod || _wcsicmp(a.name.c_str(), b.name.c_str()) != 0) {
        return false;
    }
    return a.hash != b.hash || a.hash.empty();
}

// XML entities: whether an entity's digits are all decimal (or hex) digits
bool IsDigitOf(char d, bool hex)
{
    if (hex) {
        return isxdigit(static_cast<unsigned char>(d)) != 0;
    }
    return isdigit(static_cast<unsigned char>(d)) != 0;
}

void AppendUtf8(unsigned long cp, std::string* out)
{
    if (cp < 0x80) {
        *out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        *out += static_cast<char>(0xC0 | (cp >> 6));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *out += static_cast<char>(0xE0 | (cp >> 12));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        *out += static_cast<char>(0xF0 | (cp >> 18));
        *out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        *out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        *out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// A numeric character reference without its '&' and ';' ("#65", "#x41"), appended as UTF-8. False (nothing
// appended) if it isn't one, or names no valid character.
bool DecodeCharRef(const std::string& name, std::string* out)
{
    if (name.size() <= 1 || name[0] != '#') {
        return false;
    }

    bool hex = name[1] == 'x' || name[1] == 'X';
    std::string digits = name.substr(hex ? 2 : 1);
    bool valid = !digits.empty();
    for (char d : digits) {
        valid &= IsDigitOf(d, hex);
    }
    unsigned long cp = valid ? strtoul(digits.c_str(), nullptr, hex ? 16 : 10) : 0;

    bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
    if (cp == 0 || cp > 0x10FFFF || surrogate) {
        return false;
    }
    AppendUtf8(cp, out);
    return true;
}

} // namespace

std::wstring Consent_Canonical(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return L"";
    }

    wchar_t buf[1024];
    DWORD n = GetFinalPathNameByHandleW(h, buf, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (n == 0 || n >= 1024) {
        return L"";
    }

    std::wstring s(buf, n);
    if (s.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        s = L"\\\\" + s.substr(8);
    } else if (s.rfind(L"\\\\?\\", 0) == 0) {
        s = s.substr(4);
    }
    return WithSlash(s);
}

std::wstring Consent_WorkshopRoot(const std::wstring& canonicalGameDir)
{
    // <library>\steamapps\common\DoorKickers2\ -> <library>\steamapps\workshop\content\1239080\. Steam keeps a game's
    // Workshop items in the game's own library.
    std::wstring dir = WithSlash(canonicalGameDir);
    std::wstring lower = Lower(dir);
    const std::wstring common = L"\\steamapps\\common\\";
    size_t at = lower.rfind(common);
    if (at == std::wstring::npos) {
        return L"";
    }
    bool gameFolderLast = lower.find(L'\\', at + common.size()) == lower.size() - 1; // one folder after "common"
    if (!gameFolderLast) {
        return L"";
    }
    return dir.substr(0, at) + kWorkshopContent;
}

ModSource Consent_Classify(const std::wstring& canonicalDir, const std::vector<std::wstring>& localRoots,
                           const std::wstring& workshopRoot, std::wstring* workshopId)
{
    std::wstring dir = Lower(WithSlash(canonicalDir));
    for (const auto& root : localRoots) {
        if (IsStrictlyUnder(dir, Lower(WithSlash(root)))) {
            return ModSource::Local;
        }
    }

    // Only the game's own Workshop folder counts. The prompt shows the item's real Steam link, so a lookalike path
    // elsewhere is Unknown.
    std::wstring wsRoot = Lower(WithSlash(workshopRoot)); // empty exactly when workshopRoot is
    if (IsStrictlyUnder(dir, wsRoot)) {
        std::wstring rest = dir.substr(wsRoot.size()); // "<id>\"
        if (rest.size() >= 2 && rest.back() == L'\\') {
            std::wstring id = rest.substr(0, rest.size() - 1);
            if (IsWorkshopId(id)) {
                if (workshopId) {
                    *workshopId = id;
                }
                return ModSource::Workshop;
            }
        }
    }
    return ModSource::Unknown;
}

bool Consent_ListFiles(const std::wstring& dir, std::vector<NativeFile>* files)
{
    files->clear();
    uint64_t total = 0;
    if (!ListRecursive(WithSlash(dir), L"", files, &total)) {
        return false;
    }
    std::sort(files->begin(), files->end(),
              [](const NativeFile& a, const NativeFile& b) { return Lower(a.rel) < Lower(b.rel); });
    return true;
}

// A plugin is a 64-bit DLL that exports DK2ML_PluginInit, checked on the file mapped as an image resource so nothing in
// it runs. Other DLLs in native\ are its dependencies, which the Windows loader finds in the plugin's folder.
bool Consent_ReadPlugin(const std::wstring& path, PluginManifest* manifest)
{
    if (manifest) {
        *manifest = PluginManifest();
    }
    HMODULE image = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (!image) {
        return false;
    }

    // a datafile/resource module's handle has its low bits set as a tag
    auto* base = reinterpret_cast<const BYTE*>(reinterpret_cast<uintptr_t>(image) & ~static_cast<uintptr_t>(3));
    DK2ML_Manifest raw;
    bool plugin = ReadPluginExports(base, &raw);
    FreeLibrary(image);

    if (plugin && manifest && raw.structSize) {
        manifest->present = true;
        manifest->structSize = raw.structSize;
        manifest->minApiVersion = raw.minApiVersion;
        manifest->gameVersion = raw.gameVersion;
        manifest->name = ManifestText(raw.name, sizeof(raw.name));
        manifest->version = ManifestText(raw.version, sizeof(raw.version));
        manifest->author = ManifestText(raw.author, sizeof(raw.author));
        manifest->url = ManifestText(raw.url, sizeof(raw.url));
    }
    return plugin;
}

bool Consent_IsPlugin(const std::wstring& path)
{
    return Consent_ReadPlugin(path, nullptr);
}

std::string Consent_ManifestLine(const PluginManifest& m)
{
    if (!m.present) {
        return "";
    }
    std::string line = "\"" + (m.name.empty() ? std::string("(no name)") : m.name) + "\"";
    if (!m.version.empty()) {
        line += " " + m.version;
    }
    if (!m.author.empty()) {
        line += " by " + m.author;
    }
    return line;
}

std::wstring Consent_Fingerprint(const std::wstring& dir)
{
    // every file's relative path + content, in path order: any added, removed, renamed or changed file counts
    std::vector<NativeFile> files;
    if (!Consent_ListFiles(dir, &files) || files.empty()) {
        return L"";
    }
    Sha256 outer;
    for (const auto& f : files) {
        std::string content;
        if (!ReadFileBytes(f.full, &content)) {
            return L"";
        }
        Sha256 inner;
        inner.Add(content.data(), content.size());
        unsigned char digest[kSha256Bytes];
        if (!inner.Finish(digest)) {
            return L"";
        }

        std::wstring name = Lower(f.rel);
        outer.Add(name.c_str(), (name.size() + 1) * sizeof(wchar_t));
        outer.Add(digest, sizeof(digest));
    }

    unsigned char digest[kSha256Bytes];
    return outer.Finish(digest) ? Hex(digest, sizeof(digest)) : L"";
}

std::wstring Consent_FileHash(const std::wstring& path)
{
    std::string content;
    if (!ReadFileBytes(path, &content)) {
        return L"";
    }
    Sha256 hash;
    hash.Add(content.data(), content.size());
    unsigned char digest[kSha256Bytes];
    return hash.Finish(digest) ? Hex(digest, sizeof(digest)) : L"";
}

std::vector<DllClash> Consent_FindDllClashes(const std::vector<SupportDll>& dlls)
{
    // Windows loads a DLL name once per process. The first mod's copy to load serves every plugin that imports that
    // name, so with two different files under one name, one plugin runs against the wrong one.
    std::vector<DllClash> clashes;
    for (size_t i = 0; i < dlls.size(); ++i) {
        for (size_t j = i + 1; j < dlls.size(); ++j) {
            const SupportDll& a = dlls[i];
            const SupportDll& b = dlls[j];
            if (IsClash(a, b)) {
                clashes.push_back({a.name, a.mod, b.mod});
            }
        }
    }
    return clashes;
}

namespace {

// backslashes only, and a trailing one
std::wstring ListedDir(std::wstring p)
{
    std::replace(p.begin(), p.end(), L'/', L'\\');
    return WithSlash(p);
}

} // namespace

std::wstring Consent_ModListKey(const std::wstring& path)
{
    return path.empty() ? std::wstring() : Lower(ListedDir(path));
}

EnabledDiff Consent_DiffEnabled(const std::vector<std::wstring>& known, const std::vector<std::wstring>& now)
{
    // Both sides are the game's own strings (options.xml is written from its list), so keys compare them without
    // file-system calls. Only added folders get resolved, by the caller.
    std::vector<std::wstring> keys, paths; // paths: as listed (slashes fixed), for resolving
    for (const auto& p : now) {
        std::wstring key = Consent_ModListKey(p);
        if (!key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(key);
            paths.push_back(ListedDir(p));
        }
    }

    EnabledDiff diff;
    std::vector<std::wstring> knownKeys;
    for (const auto& k : known) {
        knownKeys.push_back(Consent_ModListKey(k));
        const std::wstring& key = knownKeys.back();
        bool listed = !key.empty() && std::find(keys.begin(), keys.end(), key) != keys.end();
        diff.active.push_back(listed);
    }

    for (size_t i = 0; i < keys.size(); ++i) {
        if (std::find(knownKeys.begin(), knownKeys.end(), keys[i]) == knownKeys.end()) {
            diff.added.push_back(paths[i]);
        }
    }
    return diff;
}

std::string Consent_XmlDecode(const std::string& s)
{
    // One pass, left to right, so "&amp;apos;" is the text "&apos;", not an apostrophe. The game writes mod.xml titles
    // with entities (title="Era&apos;s ..."), and the screen escapes its text again when it builds its XML.
    static const std::pair<const char*, char> named[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}};
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        size_t semi = s[i] == '&' ? s.find(';', i + 1) : std::string::npos;
        if (semi == std::string::npos || semi - i > kMaxEntitySpan) {
            out += s[i];
            continue;
        }

        std::string name = s.substr(i + 1, semi - i - 1);
        bool decoded = false;
        for (const auto& [n, c] : named) {
            if (name == n) {
                out += c;
                decoded = true;
            }
        }
        if (!decoded) {
            decoded = DecodeCharRef(name, &out);
        }

        if (decoded) {
            i = semi;
        } else {
            out += s[i]; // not an entity this knows: kept as text
        }
    }
    return out;
}

std::wstring Consent_ModTitle(const std::wstring& modDir)
{
    // mod.xml is untrusted (declined Workshop items get here too), so it gets a bounded read and a plain scan. MSVC's
    // std::regex recurses per character and can overflow the stack on long input.
    std::string xml;
    std::string title;
    if (ReadFileHead(modDir + L"mod.xml", kMaxModXmlBytes, &xml) && FindAttribute(xml, "title", &title)) {
        title = Consent_XmlDecode(title).substr(0, kMaxTitleBytes);
        int n = MultiByteToWideChar(CP_UTF8, 0, title.data(), static_cast<int>(title.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, title.data(), static_cast<int>(title.size()), w.data(), n);
        for (auto& c : w) {
            if (c < 32) {
                c = L' ';
            }
        }
        if (!w.empty()) {
            return w;
        }
    }
    return L"(untitled)";
}

std::wstring Consent_PromptText(const std::vector<ConsentRequest>& pending)
{
    constexpr size_t kMaxListed = 15; // a message box taller than the screen hides its buttons
    std::wstring text = L"These Steam Workshop mods want to run native code (DLLs) in Door Kickers 2:\n\n";
    for (size_t i = 0; i < pending.size() && i < kMaxListed; ++i) {
        const ConsentRequest& r = pending[i];
        text += L"    \"" + r.title + L"\"  (" + (r.changed ? L"updated" : L"new") + L")  workshop item " + r.itemId +
                L"\n";
    }
    if (pending.size() > kMaxListed) {
        text += L"    and " + std::to_wstring(pending.size() - kMaxListed) + L" more\n";
    }
    text +=
        L"\nNative code runs with full access to your PC, like any program you install. Only load mods from authors "
        L"you trust.\n\n"
        L"Load them?\n"
        L"    Yes: load these versions.\n"
        L"    No: skip their code (the rest of each mod still loads).\n\n"
        L"You'll be asked again when one of them updates. Your answers are in dk2ml.ini in the game folder (delete a "
        L"line to be asked again).";
    return text;
}

bool Consent_Prompt(const std::vector<ConsentRequest>& pending)
{
    std::wstring text = Consent_PromptText(pending);
    int answer = MessageBoxW(nullptr, text.c_str(), L"Door Kickers 2 - native mods",
                             MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST | MB_SETFOREGROUND);
    return answer == IDYES;
}

void Consent_DecideAll(const std::wstring& ini, std::vector<ConsentRequest>& items, ConsentPromptFn prompt,
                       std::vector<bool>* load)
{
    load->assign(items.size(), false);
    std::vector<size_t> pendingAt;
    for (size_t i = 0; i < items.size(); ++i) {
        ConsentRequest& r = items[i];
        r.fingerprint = Consent_Fingerprint(r.nativeDir);
        if (r.fingerprint.empty()) {
            LogF("workshop item %ls: cannot read its native folder, not loading it", r.itemId.c_str());
            continue;
        }
        std::wstring stored = StoredDecision(ini, r.itemId);
        if (stored == L"allow:" + r.fingerprint) {
            LogF("workshop item %ls (%ls): allowed earlier", r.itemId.c_str(), r.title.c_str());
            (*load)[i] = true;
        } else if (stored == L"deny:" + r.fingerprint) {
            LogF("workshop item %ls (%ls): declined earlier, its code is not loaded", r.itemId.c_str(),
                 r.title.c_str());
        } else {
            r.changed = !stored.empty();
            pendingAt.push_back(i);
        }
    }

    if (pendingAt.empty()) {
        return;
    }

    std::vector<ConsentRequest> pending;
    for (size_t i : pendingAt) {
        pending.push_back(items[i]);
    }
    bool allow = prompt(pending);

    for (size_t i : pendingAt) {
        const ConsentRequest& r = items[i];
        (*load)[i] = allow;
        std::wstring value = (allow ? L"allow:" : L"deny:") + r.fingerprint;
        if (!WritePrivateProfileStringW(kSection, r.itemId.c_str(), value.c_str(), ini.c_str())) {
            LogF("cannot save the answer to %ls (error %lu); you'll be asked again", ini.c_str(), GetLastError());
        }
        LogF("workshop item %ls (%ls)%s: player %s it", r.itemId.c_str(), r.title.c_str(),
             r.changed ? " changed since the last answer" : "", allow ? "allowed" : "declined");
    }
}

void Consent_EnsureIni(const std::wstring& ini)
{
    if (GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return;
    }
    static const char kDefault[] =
        "; Door Kickers 2 native mod loader (dk2ml) settings. This file lives next to DoorKickers2.exe.\r\n"
        "[loader]\r\n"
        "; 0 turns the loader off (the game starts unmodded): the way out if a native mod keeps the game from\r\n"
        "; starting.\r\n"
        "enabled=1\r\n"
        "\r\n"
        "; Native mods (DLLs) from the Steam Workshop ask for your permission before they load, and again whenever\r\n"
        "; their code changes. 1 loads all of them without asking. Not recommended: any Workshop update could ship\r\n"
        "; anything.\r\n"
        "allow_workshop_plugins=0\r\n"
        "\r\n"
        "; Your answers, per Workshop item id: allow:<fingerprint> or deny:<fingerprint> of its native folder.\r\n"
        "; Delete a line to be asked again.\r\n"
        "[workshop]\r\n";
    std::ofstream file(ini, std::ios::binary);
    if (file) {
        file.write(kDefault, sizeof(kDefault) - 1);
    } else {
        LogF("cannot create %ls", ini.c_str());
    }
}
