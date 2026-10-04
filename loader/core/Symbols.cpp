// Resolves game functions/globals/struct layouts from DoorKickers2.pdb via the real dbghelp (System32).
// Never link dbghelp.lib here: "dbghelp.dll" in the game folder is the loader's stub (proxy/ProxyMain.cpp).
#include "Loader.h"

#include <oaidl.h> // VARIANT, for TI_GET_VALUE
#include <dbghelp.h>
#include <psapi.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Private pseudo process handle so our symbol session doesn't collide with the game's own dbghelp use (crash dumps).
HANDLE const kSymHandle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0xD2D2D2D2));

// SymTagEnum values from DIA's cvconst.h (not part of the Windows SDK)
constexpr DWORD SymTagFunction = 5;
constexpr DWORD SymTagData = 7;
constexpr DWORD SymTagPublicSymbol = 10;
constexpr DWORD SymTagUDT = 11;
constexpr DWORD SymTagEnumType = 12; // SymTagEnum
constexpr DWORD SymTagFunctionType = 13;
constexpr DWORD SymTagPointerType = 14;
constexpr DWORD SymTagArrayType = 15;
constexpr DWORD SymTagBaseType = 16;
constexpr DWORD SymTagTypedef = 17;
constexpr DWORD SymTagBaseClass = 18;
constexpr DWORD SymTagVTable = 25;
constexpr DWORD DataIsMember = 7; // DataKind, also from cvconst.h

constexpr DWORD kSymOptions =
    SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_EXACT_SYMBOLS;

constexpr int kMaxTypeDepth = 16; // base classes followed this deep, then the search gives up
constexpr int kMaxTypeNameDepth = 8; // pointer/array levels spelled out in a type name
constexpr int kDefiniteMismatch = 1 << 20; // LayoutMismatch's score for a 32-bit copy: worse than any overlap count
constexpr size_t kMaxListedNames = 8; // names or addresses a warning lists before "..."

// dbghelp is not thread safe
SRWLOCK g_lock = SRWLOCK_INIT;
DWORD64 g_base = 0;
bool g_ready = false;

decltype(&SymSetOptions) pSymSetOptions;
decltype(&SymInitializeW) pSymInitializeW;
decltype(&SymLoadModuleExW) pSymLoadModuleExW;
decltype(&SymGetModuleInfoW64) pSymGetModuleInfoW64;
decltype(&SymFromName) pSymFromName;
decltype(&SymGetTypeFromName) pSymGetTypeFromName;
decltype(&SymGetTypeInfo) pSymGetTypeInfo;

// optional: every copy of each type, explorer's type search
decltype(&SymEnumTypes) pSymEnumTypes;
// optional: per-call names at an address (fallback, --check-index)
decltype(&SymEnumSymbolsForAddr) pSymEnumSymbolsForAddr;
// optional: the name index, explorer, per-call fallback by name
decltype(&SymEnumSymbols) pSymEnumSymbols;
// optional: explorer (decorated names of overloads)
decltype(&UnDecorateSymbolName) pUnDecorateSymbolName;
// optional: naming crash addresses (crash report)
decltype(&SymFromAddr) pSymFromAddr;

bool HaveRequiredExports()
{
    return pSymSetOptions && pSymInitializeW && pSymLoadModuleExW && pSymGetModuleInfoW64 && pSymFromName &&
           pSymGetTypeFromName && pSymGetTypeInfo;
}

struct Lock {
    Lock() { AcquireSRWLockExclusive(&g_lock); }

    ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

template <typename T> bool TypeInfo(ULONG typeId, IMAGEHLP_SYMBOL_TYPE_INFO what, T* out)
{
    return pSymGetTypeInfo(kSymHandle, g_base, typeId, what, out) != FALSE;
}

std::wstring Widen(const char* s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) {
        MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    }
    return w;
}

std::string Narrow(const WCHAR* s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out.data(), n, nullptr, nullptr);
    return out;
}

bool IsFunctionSymbol(PSYMBOL_INFO sym)
{
    return sym->Tag == SymTagFunction || sym->Tag == SymTagPublicSymbol;
}

// An enumerator's value; false (out unchanged) for a non-integer variant.
bool VariantInteger(const VARIANT& v, int64_t* out)
{
    switch (v.vt) {
    case VT_I1: *out = v.cVal; break;
    case VT_UI1: *out = v.bVal; break;
    case VT_I2: *out = v.iVal; break;
    case VT_UI2: *out = v.uiVal; break;
    case VT_I4:
    case VT_INT: *out = v.lVal; break;
    case VT_UI4:
    case VT_UINT: *out = v.ulVal; break;
    case VT_I8: *out = v.llVal; break;
    case VT_UI8: *out = static_cast<int64_t>(v.ullVal); break;
    default: return false;
    }
    return true;
}

// A bitfield member: its offset is that of the storage unit it lives in.
struct Bitfield {
    bool is = false;
    DWORD position = 0; // first bit within the storage unit
    ULONG64 length = 0; // bits
};

// Searches typeId's data members (recursing into base classes) for fieldName. Returns offset or -1.
int32_t FindField(ULONG typeId, const std::wstring& fieldName, int depth, Bitfield* bits = nullptr)
{
    if (depth > kMaxTypeDepth) {
        return -1;
    }

    DWORD count = 0;
    if (!TypeInfo(typeId, TI_GET_CHILDRENCOUNT, &count) || count == 0) {
        return -1;
    }

    std::vector<char> buf(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
    auto* params = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(buf.data());
    params->Count = count;
    params->Start = 0;
    if (!TypeInfo(typeId, TI_FINDCHILDREN, params)) {
        return -1;
    }

    for (DWORD i = 0; i < count; ++i) {
        ULONG child = params->ChildId[i];
        DWORD tag = 0;
        TypeInfo(child, TI_GET_SYMTAG, &tag);

        if (tag == SymTagData) {
            WCHAR* name = nullptr;
            if (!TypeInfo(child, TI_GET_SYMNAME, &name) || !name) {
                continue;
            }
            bool match = fieldName == name;
            LocalFree(name);

            DWORD offset = 0;
            if (match && TypeInfo(child, TI_GET_OFFSET, &offset)) {
                // for a bitfield member, dbghelp gives its length in bits
                if (bits && TypeInfo(child, TI_GET_BITPOSITION, &bits->position)) {
                    bits->is = TypeInfo(child, TI_GET_LENGTH, &bits->length);
                }
                return static_cast<int32_t>(offset);
            }
        } else if (tag == SymTagBaseClass) {
            DWORD baseOffset = 0;
            ULONG baseType = 0;
            TypeInfo(child, TI_GET_OFFSET, &baseOffset);
            if (!TypeInfo(child, TI_GET_TYPEID, &baseType)) {
                continue;
            }

            int32_t inner = FindField(baseType, fieldName, depth + 1, bits);
            if (inner >= 0) {
                return static_cast<int32_t>(baseOffset) + inner;
            }
        }
    }
    return -1;
}

bool LookupType(const char* typeName, ULONG* typeId)
{
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    ZeroMemory(buf, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;
    if (!pSymGetTypeFromName(kSymHandle, g_base, typeName, sym)) {
        return false;
    }

    *typeId = sym->TypeIndex;
    return true;
}

// Many types exist more than once in the PDB with different layouts: GUI::Button is there at 1512 and 1164 bytes,
// m_pStaticText at 912 and 600; GameClient at 1078672 and 1021232. The smaller copies are 32-bit layouts (pointers
// 4 bytes, containers half the size). Nothing guarantees which copy SymGetTypeFromName returns, so every copy is found
// here and the one that fits this build is chosen (LayoutMismatch). SymEnumTypesByName returns only one copy per name,
// so every type is enumerated once (about half a second, on the first field or size lookup) and indexed by name.
struct TypeIndexData {
    std::map<std::string, std::vector<ULONG>> udts; // every copy of each struct/class/union
    std::set<std::string> enums; // for the explorer's type search
};

const TypeIndexData& TypeIndex()
{
    static TypeIndexData index;
    static bool built = false;
    if (built || !pSymEnumTypes) {
        return index;
    }

    built = true;
    auto callback = [](PSYMBOL_INFO sym, ULONG, PVOID user) -> BOOL {
        auto* data = static_cast<TypeIndexData*>(user);
        if (sym->Tag == SymTagUDT && sym->Size != 0) { // not forward declarations
            auto& ids = data->udts[sym->Name];
            if (std::find(ids.begin(), ids.end(), sym->TypeIndex) == ids.end()) {
                ids.push_back(sym->TypeIndex);
            }
        } else if (sym->Tag == SymTagEnumType) {
            data->enums.insert(sym->Name);
        }
        return TRUE;
    };
    pSymEnumTypes(kSymHandle, g_base, callback, &index);
    return index;
}

const std::vector<ULONG>& TypeCopies(const char* typeName)
{
    static const std::vector<ULONG> none;
    const auto& udts = TypeIndex().udts;
    auto it = udts.find(typeName);
    return it != udts.end() ? it->second : none;
}

// 8 or 4 if child is a data member (not static) whose type is a pointer, else 0.
int MemberPointerSize(ULONG child)
{
    DWORD tag = 0;
    DWORD kind = 0;
    if (!TypeInfo(child, TI_GET_SYMTAG, &tag) || tag != SymTagData) {
        return 0;
    }
    if (!TypeInfo(child, TI_GET_DATAKIND, &kind) || kind != DataIsMember) {
        return 0;
    }

    ULONG type = 0;
    DWORD typeTag = 0;
    if (!TypeInfo(child, TI_GET_TYPEID, &type) || !TypeInfo(type, TI_GET_SYMTAG, &typeTag)) {
        return 0;
    }
    if (typeTag != SymTagPointerType) {
        return 0;
    }

    ULONG64 length = 0;
    if (!TypeInfo(type, TI_GET_LENGTH, &length) || (length != 4 && length != 8)) {
        return 0;
    }
    return static_cast<int>(length);
}

// 8 or 4: the size of the first pointer in the type's data members (base classes included), 0 if it has none.
// The PDB also holds 32-bit copies of many game types (pointers 4 bytes, every offset after one shifted), which are
// never the layout the 64-bit game uses.
int PointerSize(ULONG typeId, int depth)
{
    if (depth > kMaxTypeDepth) {
        return 0;
    }

    DWORD count = 0;
    if (!TypeInfo(typeId, TI_GET_CHILDRENCOUNT, &count) || count == 0) {
        return 0;
    }

    std::vector<char> buf(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
    auto* params = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(buf.data());
    params->Count = count;
    params->Start = 0;
    if (!TypeInfo(typeId, TI_FINDCHILDREN, params)) {
        return 0;
    }

    // own members first: a base class can be one record both layouts share
    for (DWORD i = 0; i < count; ++i) {
        if (int size = MemberPointerSize(params->ChildId[i])) {
            return size;
        }
    }

    for (DWORD i = 0; i < count; ++i) {
        ULONG child = params->ChildId[i];
        ULONG type = 0;
        DWORD tag = 0;
        if (TypeInfo(child, TI_GET_SYMTAG, &tag) && tag == SymTagBaseClass && TypeInfo(child, TI_GET_TYPEID, &type)) {
            if (int size = PointerSize(type, depth + 1)) {
                return size;
            }
        }
    }
    return 0;
}

// A child's tag, offset and the size of its type; false if dbghelp doesn't give all of them.
bool ChildPlacement(ULONG child, DWORD* tag, DWORD* offset, ULONG64* length)
{
    ULONG type = 0;
    if (!TypeInfo(child, TI_GET_SYMTAG, tag) || !TypeInfo(child, TI_GET_TYPEID, &type)) {
        return false;
    }
    return TypeInfo(child, TI_GET_OFFSET, offset) && TypeInfo(type, TI_GET_LENGTH, length);
}

// A data member that has storage of its own: not static, not a bitfield (bitfields share their storage).
bool IsPlainDataMember(ULONG child)
{
    DWORD kind = 0;
    DWORD bit = 0;
    if (!TypeInfo(child, TI_GET_DATAKIND, &kind) || kind != DataIsMember) {
        return false;
    }
    return !TypeInfo(child, TI_GET_BITPOSITION, &bit);
}

// How badly a copy's layout fits this (64-bit) build: the number of data members and bases that overlap the next one
// when sized with the type sizes dbghelp gives here, plus one if the last runs past the type's size. A 32-bit copy
// (containers 16 bytes there, 32 here) overlaps all over. The real one overlaps only where the class itself overlaps
// members (anonymous unions), which every copy of it shares. A 32-bit pointer counts as a definite mismatch.
int LayoutMismatch(ULONG typeId)
{
    if (PointerSize(typeId, 0) == 4) {
        return kDefiniteMismatch;
    }

    DWORD count = 0;
    if (!TypeInfo(typeId, TI_GET_CHILDRENCOUNT, &count) || count == 0) {
        return 0;
    }

    std::vector<char> buf(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
    auto* params = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(buf.data());
    params->Count = count;
    params->Start = 0;
    if (!TypeInfo(typeId, TI_FINDCHILDREN, params)) {
        return 0;
    }

    std::vector<std::pair<uint64_t, uint64_t>> spans; // offset, size
    for (DWORD i = 0; i < count; ++i) {
        ULONG child = params->ChildId[i];
        DWORD tag = 0;
        DWORD offset = 0;
        ULONG64 length = 0;
        if (!ChildPlacement(child, &tag, &offset, &length) || length == 0) {
            continue;
        }

        bool member = tag == SymTagData && IsPlainDataMember(child);
        if (member || tag == SymTagBaseClass) {
            spans.push_back({offset, length});
        }
    }
    std::sort(spans.begin(), spans.end());

    int overlaps = 0;
    for (size_t i = 0; i + 1 < spans.size(); ++i) {
        if (spans[i + 1].first < spans[i].first + spans[i].second) {
            ++overlaps;
        }
    }
    if (!spans.empty()) {
        uint64_t end = spans.back().first + spans.back().second;
        ULONG64 size = 0;
        if (TypeInfo(typeId, TI_GET_LENGTH, &size) && end > size) {
            ++overlaps;
        }
    }
    return overlaps;
}

// The copy of a type to use: the one that fits this build best, SymGetTypeFromName's pick on a tie. Also returns
// the other copies that fit equally well (these must agree with it; anything else is a stale layout).
bool ChooseType(const char* typeName, ULONG* typeId, std::vector<ULONG>* equals = nullptr)
{
    if (!LookupType(typeName, typeId)) {
        return false;
    }

    int best = LayoutMismatch(*typeId);
    std::vector<std::pair<ULONG, int>> scored;
    for (ULONG copy : TypeCopies(typeName)) {
        if (copy == *typeId) {
            continue;
        }
        int score = LayoutMismatch(copy);
        scored.push_back({copy, score});
        if (score < best) {
            best = score;
            *typeId = copy;
        }
    }

    if (equals) {
        equals->clear();
        for (auto& [copy, score] : scored) {
            if (copy != *typeId && score == best) {
                equals->push_back(copy);
            }
        }
    }
    return true;
}

// Logged once per type (and field).
bool FirstReport(const std::string& key)
{
    static std::set<std::string> reported;
    return reported.insert(key).second;
}

std::string Hex(uint64_t v)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

// --- function names by address and addresses by name ---
//
// Per-call dbghelp name questions are slow on this PDB, whatever the answer (build 112): SymEnumSymbolsForAddr
// ~170 ms, SymEnumSymbols(name) ~30 ms. One SymEnumSymbols("*") pass takes ~250 ms. So the first question builds an
// index of both directions in one pass, and it's kept because the PDB never changes. It serves the folded-code check
// (every hook), the overload check (every undecorated name), Plugins_LogSharedHooks, the safe-hook target namer
// (Symbols_NameAt, in skip log lines) and the explorer. The per-call versions are the fallback and the reference
// symtest --check-index compares the index with.

// The distinct addresses of the functions named exactly `name`. Call with g_lock held.
std::vector<DWORD64> AddressesOfSlow(const char* name)
{
    struct Context {
        const char* name;
        std::vector<DWORD64> addresses;
    } context = {name, {}};

    if (!pSymEnumSymbols) {
        return context.addresses;
    }

    auto callback = [](PSYMBOL_INFO sym, ULONG, PVOID user) -> BOOL {
        auto* c = static_cast<Context*>(user);
        if (!IsFunctionSymbol(sym)) {
            return TRUE;
        }
        // dbghelp's mask treats '*'/'?' as wildcards and its case handling depends on the symbol options, so compare
        // exactly
        if (std::string(sym->Name, strnlen(sym->Name, sym->NameLen)) != c->name) {
            return TRUE;
        }
        if (std::find(c->addresses.begin(), c->addresses.end(), sym->Address) == c->addresses.end()) {
            c->addresses.push_back(sym->Address);
        }
        return TRUE;
    };
    pSymEnumSymbols(kSymHandle, g_base, name, callback, &context);
    return context.addresses;
}

// Every function name at an address: more than one when the linker folded identical functions into one (ICF keeps one
// function symbol and a public symbol for every name folded into it). decorated: the publics' decorated names
// instead. Call with g_lock held.
std::vector<std::string> NamesAtSlow(DWORD64 address, bool decorated)
{
    struct Context {
        DWORD64 address;
        std::vector<std::string> names;
    } context = {address, {}};

    if (!pSymEnumSymbolsForAddr) {
        return context.names;
    }

    auto callback = [](PSYMBOL_INFO sym, ULONG, PVOID user) -> BOOL {
        auto* c = static_cast<Context*>(user);
        if (sym->Address == c->address && IsFunctionSymbol(sym)) {
            std::string name(sym->Name, strnlen(sym->Name, sym->NameLen));
            if (std::find(c->names.begin(), c->names.end(), name) == c->names.end()) {
                c->names.push_back(name);
            }
        }
        return TRUE;
    };

    // the function record has only the plain name, so the decorated one comes from the publics alone
    if (decorated) {
        pSymSetOptions((kSymOptions & ~SYMOPT_UNDNAME) | SYMOPT_PUBLICS_ONLY);
    }
    pSymEnumSymbolsForAddr(kSymHandle, address, callback, &context);
    if (decorated) {
        pSymSetOptions(kSymOptions);
    }
    return context.names;
}

struct NameIndex {
    bool built = false; // tried
    bool usable = false; // the pass found functions
    std::unordered_map<DWORD64, std::vector<std::string>> namesAt;
    std::unordered_map<std::string, std::vector<DWORD64>> addressesOf;
};

NameIndex g_plainIndex;
NameIndex g_decoratedIndex; // only symtest's explorer asks for decorated names

// One pass over every function and public symbol, with the same options NamesAtSlow uses. Call with g_lock held.
NameIndex& Index(bool decorated)
{
    NameIndex& index = decorated ? g_decoratedIndex : g_plainIndex;
    if (index.built || !pSymEnumSymbols) {
        return index;
    }

    index.built = true;
    ULONGLONG started = GetTickCount64();

    // the function record's name first, as SymEnumSymbolsForAddr has it; then the publics in the order they come
    struct Context {
        std::unordered_map<DWORD64, std::vector<std::string>> functions, publics;
        std::unordered_map<std::string, std::vector<DWORD64>>* addressesOf;
    } context = {{}, {}, &index.addressesOf};

    auto callback = [](PSYMBOL_INFO sym, ULONG, PVOID user) -> BOOL {
        auto* c = static_cast<Context*>(user);
        if (!IsFunctionSymbol(sym)) {
            return TRUE;
        }

        std::string name(sym->Name, strnlen(sym->Name, sym->NameLen));
        std::vector<std::string>& names = (sym->Tag == SymTagFunction ? c->functions : c->publics)[sym->Address];
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }

        std::vector<DWORD64>& addresses = (*c->addressesOf)[name];
        if (std::find(addresses.begin(), addresses.end(), sym->Address) == addresses.end()) {
            addresses.push_back(sym->Address);
        }
        return TRUE;
    };
    if (decorated) {
        pSymSetOptions((kSymOptions & ~SYMOPT_UNDNAME) | SYMOPT_PUBLICS_ONLY);
    }
    pSymEnumSymbols(kSymHandle, g_base, "*", callback, &context);
    if (decorated) {
        pSymSetOptions(kSymOptions);
    }

    index.namesAt = std::move(context.functions);
    for (auto& [address, names] : context.publics) {
        std::vector<std::string>& all = index.namesAt[address];
        for (auto& name : names) {
            if (std::find(all.begin(), all.end(), name) == all.end()) {
                all.push_back(std::move(name));
            }
        }
    }
    index.usable = !index.namesAt.empty();

    LogF("symbol index%s: %zu names at %zu addresses (%llu ms)", decorated ? " (decorated)" : "",
         index.addressesOf.size(), index.namesAt.size(), static_cast<unsigned long long>(GetTickCount64() - started));
    return index;
}

std::vector<std::string> NamesAt(DWORD64 address, bool decorated)
{
    NameIndex& index = Index(decorated);
    if (!index.usable) {
        return NamesAtSlow(address, decorated);
    }

    auto it = index.namesAt.find(address);
    return it != index.namesAt.end() ? it->second : std::vector<std::string>();
}

std::vector<DWORD64> AddressesOf(const char* name)
{
    NameIndex& index = Index(false);
    if (!index.usable) {
        return AddressesOfSlow(name);
    }

    auto it = index.addressesOf.find(name);
    return it != index.addressesOf.end() ? it->second : std::vector<DWORD64>();
}

// An undecorated name can stand for several functions (overloads, or a static helper of the same name in several
// files). SymFromName then returns one of them, and which one isn't defined. Checked once per name. Call with g_lock
// held.
void WarnIfOverloaded(const char* name, DWORD64 chosen)
{
    if (!FirstReport(std::string("overloads of ") + name)) {
        return;
    }
    std::vector<DWORD64> addresses = AddressesOf(name);
    if (addresses.size() < 2) {
        return;
    }

    std::string list;
    for (size_t i = 0; i < addresses.size() && i < kMaxListedNames; ++i) {
        list += (i ? ", " : "") + Hex(addresses[i]);
    }
    if (addresses.size() > kMaxListedNames) {
        list += ", ...";
    }

    LogF(
        "WARNING: %s is %zu different functions (overloads?): %s. ResolveSymbol returned %s, which isn't guaranteed to "
        "be the one you mean: pass the decorated name (symtest --find shows it) to pick one.",
        name, addresses.size(), list.c_str(), Hex(chosen).c_str());
}

} // namespace

bool Symbols_Init(const std::wstring& gameDir, HMODULE exe)
{
    Lock lock;

    HMODULE dbghelp = RealDbghelp();
    if (!dbghelp) {
        LogF("cannot load System32\\dbghelp.dll");
        return false;
    }

    pSymSetOptions = reinterpret_cast<decltype(pSymSetOptions)>(GetProcAddress(dbghelp, "SymSetOptions"));
    pSymInitializeW = reinterpret_cast<decltype(pSymInitializeW)>(GetProcAddress(dbghelp, "SymInitializeW"));
    pSymLoadModuleExW = reinterpret_cast<decltype(pSymLoadModuleExW)>(GetProcAddress(dbghelp, "SymLoadModuleExW"));
    pSymGetModuleInfoW64 =
        reinterpret_cast<decltype(pSymGetModuleInfoW64)>(GetProcAddress(dbghelp, "SymGetModuleInfoW64"));
    pSymFromName = reinterpret_cast<decltype(pSymFromName)>(GetProcAddress(dbghelp, "SymFromName"));
    pSymGetTypeFromName =
        reinterpret_cast<decltype(pSymGetTypeFromName)>(GetProcAddress(dbghelp, "SymGetTypeFromName"));
    pSymGetTypeInfo = reinterpret_cast<decltype(pSymGetTypeInfo)>(GetProcAddress(dbghelp, "SymGetTypeInfo"));

    pSymEnumTypes = reinterpret_cast<decltype(pSymEnumTypes)>(GetProcAddress(dbghelp, "SymEnumTypes"));
    pSymEnumSymbolsForAddr =
        reinterpret_cast<decltype(pSymEnumSymbolsForAddr)>(GetProcAddress(dbghelp, "SymEnumSymbolsForAddr"));
    pSymEnumSymbols = reinterpret_cast<decltype(pSymEnumSymbols)>(GetProcAddress(dbghelp, "SymEnumSymbols"));
    pUnDecorateSymbolName =
        reinterpret_cast<decltype(pUnDecorateSymbolName)>(GetProcAddress(dbghelp, "UnDecorateSymbolName"));
    pSymFromAddr = reinterpret_cast<decltype(pSymFromAddr)>(GetProcAddress(dbghelp, "SymFromAddr"));

    if (!HaveRequiredExports()) {
        LogF("System32\\dbghelp.dll is missing required exports");
        return false;
    }

    pSymSetOptions(kSymOptions);
    if (!pSymInitializeW(kSymHandle, gameDir.c_str(), FALSE)) {
        LogF("SymInitialize failed (error %lu)", GetLastError());
        return false;
    }

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(exe, exePath, MAX_PATH);
    MODULEINFO mi = {};
    GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof(mi));

    g_base = pSymLoadModuleExW(kSymHandle, nullptr, exePath, nullptr, reinterpret_cast<DWORD64>(exe), mi.SizeOfImage,
                               nullptr, 0);
    if (!g_base) {
        LogF("SymLoadModuleEx failed for %ls (error %lu)", exePath, GetLastError());
        return false;
    }

    IMAGEHLP_MODULEW64 info = {};
    info.SizeOfStruct = sizeof(info);
    pSymGetModuleInfoW64(kSymHandle, g_base, &info);
    if (info.SymType != SymPdb) {
        // with SYMOPT_DEFERRED_LOADS the first query loads the pdb; force it now so failures show up here
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = MAX_SYM_NAME;
        pSymFromName(kSymHandle, "WinMain", sym);
        pSymGetModuleInfoW64(kSymHandle, g_base, &info);
    }
    if (info.SymType != SymPdb) {
        LogF("DoorKickers2.pdb did not load (SymType=%d). Is it missing or out of date with the exe?", info.SymType);
        return false;
    }

    LogF("symbols loaded from %ls", info.LoadedPdbName);
    g_ready = true;
    return true;
}

void* Symbols_Resolve(const char* name)
{
    if (!g_ready || !name) {
        return nullptr;
    }

    Lock lock;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;

    // A decorated name ("?Func@Class@@...") picks one exact overload. Those only match with undecoration off.
    bool decorated = name[0] == '?';
    if (decorated) {
        pSymSetOptions(kSymOptions & ~SYMOPT_UNDNAME);
    }
    BOOL found = pSymFromName(kSymHandle, name, sym);
    if (decorated) {
        pSymSetOptions(kSymOptions);
    }

    if (!found) {
        return nullptr;
    }
    if (!decorated) {
        WarnIfOverloaded(name, sym->Address);
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(sym->Address));
}

int32_t Symbols_FieldOffset(const char* typeName, const char* fieldName)
{
    if (!g_ready || !typeName || !fieldName) {
        return -1;
    }

    Lock lock;
    ULONG typeId = 0;
    std::vector<ULONG> equals;
    if (!ChooseType(typeName, &typeId, &equals)) {
        return -1;
    }

    std::wstring field = Widen(fieldName);
    Bitfield bits;
    int32_t offset = FindField(typeId, field, 0, &bits);
    if (bits.is && FirstReport(std::string("bitfield ") + typeName + "::" + fieldName)) {
        LogF("WARNING: %s::%s is a bitfield: bits %lu-%llu of the storage at offset %d (%llu bits). GetFieldOffset "
             "gives only that offset: shift and mask the value yourself.",
             typeName, fieldName, bits.position, bits.position + bits.length - 1, offset, bits.length);
    }

    // other copies that fit this build as well, if any, must agree
    std::string others;
    for (ULONG copy : equals) {
        int32_t other = FindField(copy, field, 0);
        if (other != offset) {
            others += (others.empty() ? "" : ", ") + std::to_string(other);
        }
    }
    if (!others.empty() && FirstReport(std::string(typeName) + "::" + fieldName)) {
        LogF("WARNING: %s exists more than once in the PDB with different layouts that both fit this build: %s::%s is "
             "at %d in the copy used, %s in the other(s). Check which one the game uses (symtest, in game).",
             typeName, typeName, fieldName, offset, others.c_str());
    }
    return offset;
}

bool Symbols_EnumValue(const char* enumType, const char* name, int64_t* out)
{
    if (!g_ready || !enumType || !name || !out) {
        return false;
    }

    Lock lock;
    ULONG typeId = 0;
    if (!LookupType(enumType, &typeId)) {
        return false;
    }
    DWORD count = 0;
    if (!TypeInfo(typeId, TI_GET_CHILDRENCOUNT, &count) || count == 0) {
        return false;
    }

    std::vector<char> buf(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
    auto* params = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(buf.data());
    params->Count = count;
    params->Start = 0;
    if (!TypeInfo(typeId, TI_FINDCHILDREN, params)) {
        return false;
    }

    std::wstring wanted = Widen(name);
    for (DWORD i = 0; i < count; ++i) {
        WCHAR* childName = nullptr;
        if (!TypeInfo(params->ChildId[i], TI_GET_SYMNAME, &childName) || !childName) {
            continue;
        }
        bool match = wanted == childName;
        LocalFree(childName);
        if (!match) {
            continue;
        }

        VARIANT v = {}; // integer variants only, so no VariantClear needed
        if (!TypeInfo(params->ChildId[i], TI_GET_VALUE, &v)) {
            return false;
        }
        return VariantInteger(v, out);
    }
    return false;
}

uint32_t Symbols_TypeSize(const char* typeName)
{
    if (!g_ready || !typeName) {
        return 0;
    }

    Lock lock;
    ULONG typeId = 0;
    ULONG64 length = 0;
    std::vector<ULONG> equals;
    if (!ChooseType(typeName, &typeId, &equals) || !TypeInfo(typeId, TI_GET_LENGTH, &length)) {
        return 0;
    }

    std::string others;
    for (ULONG copy : equals) {
        ULONG64 other = 0;
        if (TypeInfo(copy, TI_GET_LENGTH, &other) && other != length) {
            others += (others.empty() ? "" : ", ") + std::to_string(other);
        }
    }
    if (!others.empty() && FirstReport(typeName)) {
        LogF("WARNING: %s exists more than once in the PDB with different sizes that both fit this build: %llu in the "
             "copy used, %s in the other(s)",
             typeName, static_cast<unsigned long long>(length), others.c_str());
    }
    return static_cast<uint32_t>(length);
}

int Symbols_WarnIfShared(void* address, const char* who)
{
    if (!g_ready || !address) {
        return 1;
    }

    std::vector<std::string> names;
    {
        Lock lock;
        names = NamesAt(reinterpret_cast<DWORD64>(address), false);
    }

    if (names.size() > 1) {
        std::string list;
        for (size_t i = 0; i < names.size() && i < kMaxListedNames; ++i) {
            list += (i ? ", " : "") + names[i];
        }
        if (names.size() > kMaxListedNames) {
            list += ", ...";
        }
        LogF("WARNING (%s): %s is %zu functions the linker folded into one (identical code): %s. A hook on it runs "
             "for every one of them; check the caller (e.g. rcx/this) in the callback.",
             who ? who : "?", Hex(reinterpret_cast<DWORD64>(address)).c_str(), names.size(), list.c_str());
    }
    return names.empty() ? 1 : static_cast<int>(names.size());
}

std::vector<std::string> Symbols_NamesAt(void* address)
{
    if (!g_ready || !address) {
        return {};
    }

    Lock lock;
    return NamesAt(reinterpret_cast<DWORD64>(address), false);
}

bool Symbols_DescribeTry(uintptr_t address, char* out, size_t size)
{
    if (!g_ready || !address || !pSymFromAddr || !size) {
        return false;
    }
    // the crash report: the crashing thread may hold the lock (a plugin's lookup), and the heap may be broken
    if (!TryAcquireSRWLockExclusive(&g_lock)) {
        return false;
    }

    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    bool found = pSymFromAddr(kSymHandle, address, &displacement, sym) != FALSE;
    if (found) {
        _snprintf_s(out, size, _TRUNCATE, "%.*s+0x%llX", static_cast<int>(strnlen(sym->Name, sym->NameLen)), sym->Name,
                    static_cast<unsigned long long>(displacement));
    }

    ReleaseSRWLockExclusive(&g_lock);
    return found;
}

std::string Symbols_NameAt(void* address)
{
    std::vector<std::string> names = Symbols_NamesAt(address);
    return names.empty() ? "" : names[0];
}

int Symbols_CheckIndex(const std::vector<std::string>& names, size_t samples)
{
    if (!g_ready) {
        return -1;
    }

    Lock lock;
    NameIndex& index = Index(false);
    if (!index.usable) {
        LogF("index check: no index (SymEnumSymbols missing, or it found no functions)");
        return -1;
    }

    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (const auto& x : v) {
            s += (s.empty() ? "" : ", ") + x;
        }
        return "[" + s + "]";
    };
    auto spread = [](const auto& all, size_t count) {
        std::remove_const_t<std::remove_reference_t<decltype(all)>> picked;
        size_t step = std::max<size_t>(1, all.size() / std::max<size_t>(1, count));
        for (size_t i = 0; i < all.size() && picked.size() < count; i += step) {
            picked.push_back(all[i]);
        }
        return picked;
    };

    // addresses: the named functions', then folded and single ones spread over the index
    std::vector<DWORD64> addresses;
    std::vector<DWORD64> folded;
    std::vector<DWORD64> single;
    for (const auto& name : names) {
        alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + MAX_SYM_NAME] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = MAX_SYM_NAME;
        if (pSymFromName(kSymHandle, name.c_str(), sym)) {
            addresses.push_back(sym->Address);
        } else {
            LogF("index check: %s not found", name.c_str());
        }
    }
    for (const auto& [address, at] : index.namesAt) {
        (at.size() > 1 ? folded : single).push_back(address);
    }
    std::sort(folded.begin(), folded.end());
    std::sort(single.begin(), single.end());
    for (auto* part : {&folded, &single}) {
        for (DWORD64 a : spread(*part, samples)) {
            addresses.push_back(a);
        }
    }

    // names: the given ones, then overloaded and unique ones spread over the index
    std::vector<std::string> checkNames(names);
    std::vector<std::string> overloaded;
    std::vector<std::string> unique;
    for (const auto& [name, at] : index.addressesOf) {
        (at.size() > 1 ? overloaded : unique).push_back(name);
    }
    std::sort(overloaded.begin(), overloaded.end());
    std::sort(unique.begin(), unique.end());
    for (auto* part : {&overloaded, &unique}) {
        for (const auto& n : spread(*part, samples)) {
            checkNames.push_back(n);
        }
    }

    int mismatches = 0;
    for (DWORD64 a : addresses) {
        std::vector<std::string> slow = NamesAtSlow(a, false);
        std::vector<std::string> fast;
        if (auto it = index.namesAt.find(a); it != index.namesAt.end()) {
            fast = it->second;
        }

        std::vector<std::string> slowSorted = slow;
        std::vector<std::string> fastSorted = fast;
        std::sort(slowSorted.begin(), slowSorted.end());
        std::sort(fastSorted.begin(), fastSorted.end());
        bool sameFirst = slow.empty() == fast.empty() && (slow.empty() || slow[0] == fast[0]);
        if (slowSorted != fastSorted || !sameFirst) {
            ++mismatches;
            LogF("index check: names at %s: index %s, dbghelp %s", Hex(a).c_str(), join(fast).c_str(),
                 join(slow).c_str());
        }
    }

    int maskMisses = 0;
    for (const auto& n : checkNames) {
        std::vector<DWORD64> slow = AddressesOfSlow(n.c_str());
        std::vector<DWORD64> fast;
        if (auto it = index.addressesOf.find(n); it != index.addressesOf.end()) {
            fast = it->second;
        }

        std::sort(slow.begin(), slow.end());
        std::sort(fast.begin(), fast.end());
        if (slow.empty() && !fast.empty()) {
            // dbghelp's name mask finds nothing for some names (operator+=, `RTTI...', a leading underscore, ...),
            // and the index finds them
            ++maskMisses;
            continue;
        }
        if (slow != fast) {
            ++mismatches;
            LogF("index check: %s: %zu address(es) in the index, %zu from dbghelp", n.c_str(), fast.size(),
                 slow.size());
        }
    }

    LogF("index check: %zu addresses (%zu folded in the index), %zu names (%d only the index finds): %d mismatch(es)",
         addresses.size(), folded.size(), checkNames.size(), maskMisses, mismatches);
    return mismatches;
}

// --- explorer (symtest --find / --types / --type / --enum) ---

namespace {

bool SameCharIgnoringCase(char a, char b)
{
    return tolower(static_cast<unsigned char>(a)) == tolower(static_cast<unsigned char>(b));
}

// '*' any run, '?' any one character; case-insensitive
bool WildcardMatch(const char* pattern, const char* text)
{
    const char* star = nullptr;
    const char* resume = nullptr;
    while (*text) {
        if (*pattern == '*') {
            star = pattern++;
            resume = text;
        } else if (*pattern == '?' || SameCharIgnoringCase(*pattern, *text)) {
            ++pattern;
            ++text;
        } else if (star) {
            pattern = star + 1;
            text = ++resume;
        } else {
            return false;
        }
    }

    while (*pattern == '*') {
        ++pattern;
    }
    return *pattern == '\0';
}

// BasicType values from DIA's cvconst.h
std::string BasicTypeName(DWORD basicType, ULONG64 length)
{
    auto sized = [&](const char* s1, const char* s2, const char* s4, const char* s8) {
        if (length == 1) {
            return std::string(s1);
        }
        if (length == 2) {
            return std::string(s2);
        }
        if (length == 4) {
            return std::string(s4);
        }
        return std::string(s8);
    };

    switch (basicType) {
    case 1: return "void";
    case 2: return "char";
    case 3: return "wchar_t";
    case 6: return sized("int8_t", "int16_t", "int32_t", "int64_t");
    case 7: return sized("uint8_t", "uint16_t", "uint32_t", "uint64_t");
    case 8: return length == 4 ? "float" : "double";
    case 10: return "bool";
    case 13: return "long";
    case 14: return "unsigned long";
    case 31: return "char16_t";
    case 32: return "char32_t";
    }
    return "<" + std::to_string(length) + "-byte basic type>";
}

std::string TypeName(ULONG typeId, int depth = 0)
{
    DWORD tag = 0;
    if (depth > kMaxTypeNameDepth || !TypeInfo(typeId, TI_GET_SYMTAG, &tag)) {
        return "?";
    }

    switch (tag) {
    case SymTagBaseType: {
        DWORD basic = 0;
        ULONG64 length = 0;
        TypeInfo(typeId, TI_GET_BASETYPE, &basic);
        TypeInfo(typeId, TI_GET_LENGTH, &length);
        return BasicTypeName(basic, length);
    }
    case SymTagPointerType: {
        ULONG pointee = 0;
        BOOL reference = FALSE;
        TypeInfo(typeId, TI_GET_TYPEID, &pointee);
        TypeInfo(typeId, TI_GET_IS_REFERENCE, &reference);
        return TypeName(pointee, depth + 1) + (reference ? "&" : "*");
    }
    case SymTagArrayType: {
        ULONG element = 0;
        DWORD count = 0;
        TypeInfo(typeId, TI_GET_TYPEID, &element);
        TypeInfo(typeId, TI_GET_COUNT, &count);
        return TypeName(element, depth + 1) + "[" + std::to_string(count) + "]";
    }
    case SymTagFunctionType: return "function";
    case SymTagUDT:
    case SymTagEnumType:
    case SymTagTypedef: {
        WCHAR* name = nullptr;
        if (!TypeInfo(typeId, TI_GET_SYMNAME, &name) || !name) {
            return "?";
        }
        std::string s = Narrow(name);
        LocalFree(name);
        return s;
    }
    }
    return "?";
}

// decorated is the public name of an overload of plainName ("?Func@Class@@..." for "Class::Func")
bool IsDecoratedFormOf(const std::string& decorated, const std::string& plainName)
{
    if (decorated[0] != '?' || !pUnDecorateSymbolName) {
        return false;
    }
    char plain[MAX_SYM_NAME] = {};
    return pUnDecorateSymbolName(decorated.c_str(), plain, MAX_SYM_NAME, UNDNAME_NAME_ONLY) && plainName == plain;
}

// One member of a type for the explorer: a data member, a base class or the vtable pointer. False for anything else
// (static members, methods, nested types, typedefs).
bool DescribeMember(ULONG child, TypeMember* m)
{
    DWORD tag = 0;
    if (!TypeInfo(child, TI_GET_SYMTAG, &tag)) {
        return false;
    }

    ULONG type = 0;
    if (tag == SymTagData) {
        DWORD kind = 0;
        if (!TypeInfo(child, TI_GET_DATAKIND, &kind) || kind != DataIsMember) {
            return false; // static members and the like live elsewhere
        }

        WCHAR* name = nullptr;
        if (TypeInfo(child, TI_GET_SYMNAME, &name) && name) {
            m->name = Narrow(name);
            LocalFree(name);
        }
        TypeInfo(child, TI_GET_TYPEID, &type);
        m->type = TypeName(type);
        TypeInfo(type, TI_GET_LENGTH, &m->size);

        DWORD bit = 0;
        ULONG64 bits = 0;
        if (TypeInfo(child, TI_GET_BITPOSITION, &bit) && TypeInfo(child, TI_GET_LENGTH, &bits)) {
            m->bitfield = true;
            m->bitPosition = bit;
            m->bitLength = static_cast<uint32_t>(bits);
        }
    } else if (tag == SymTagBaseClass) {
        TypeInfo(child, TI_GET_TYPEID, &type);
        m->base = true;
        m->name = "(base class)";
        m->type = TypeName(type);
        TypeInfo(type, TI_GET_LENGTH, &m->size);
    } else if (tag == SymTagVTable) {
        m->name = "(vtable pointer)";
        m->type = "void*";
        m->size = sizeof(void*);
    } else {
        return false; // methods, nested types, typedefs
    }

    DWORD offset = 0;
    TypeInfo(child, TI_GET_OFFSET, &offset);
    m->offset = static_cast<int32_t>(offset);
    return true;
}

} // namespace

std::vector<FoundSymbol> Symbols_Find(const char* mask, size_t limit)
{
    std::vector<FoundSymbol> found;
    if (!g_ready || !mask || !pSymEnumSymbols) {
        return found;
    }

    Lock lock;

    struct Context {
        const char* mask;
        std::vector<FoundSymbol>* found;
        size_t limit;
    } context = {mask, &found, limit};

    auto callback = [](PSYMBOL_INFO sym, ULONG, PVOID user) -> BOOL {
        auto* c = static_cast<Context*>(user);
        if (c->found->size() > c->limit) {
            return FALSE; // the caller shows that there are more
        }

        std::string name(sym->Name, strnlen(sym->Name, sym->NameLen));
        bool code = sym->Tag == SymTagFunction;
        bool data = sym->Tag == SymTagData;
        if (!code && !data && sym->Tag != SymTagPublicSymbol) {
            return TRUE;
        }
        if (data && sym->Flags & (SYMFLAG_LOCAL | SYMFLAG_PARAMETER | SYMFLAG_REGREL)) {
            return TRUE; // locals of functions, not globals
        }
        if (!WildcardMatch(c->mask, name.c_str())) { // matched here, case-insensitive, whatever dbghelp's mask rules
            return TRUE;
        }

        for (auto& f : *c->found) {
            if (f.address == sym->Address && f.name == name) {
                if (code || data) { // the function/data record says more than the public one
                    f.kind = code ? FoundSymbol::Function : FoundSymbol::Global;
                }
                return TRUE;
            }
        }

        FoundSymbol::Kind kind = FoundSymbol::Public;
        if (code) {
            kind = FoundSymbol::Function;
        } else if (data) {
            kind = FoundSymbol::Global;
        }
        c->found->push_back({name, "", sym->Address, kind, 1});
        return TRUE;
    };
    pSymEnumSymbols(kSymHandle, g_base, "*", callback, &context);

    // decorated names (to pick an overload) and folded names, from the public symbols at each address
    for (auto& f : found) {
        std::vector<std::string> decorated = NamesAt(f.address, true);
        f.foldedWith = static_cast<int>(NamesAt(f.address, false).size());
        for (const auto& d : decorated) {
            if (IsDecoratedFormOf(d, f.name)) {
                f.decorated = d;
                break;
            }
        }
    }

    std::sort(found.begin(), found.end(), [](const FoundSymbol& a, const FoundSymbol& b) {
        return a.name != b.name ? a.name < b.name : a.address < b.address;
    });
    return found;
}

std::vector<std::string> Symbols_FindTypes(const char* mask)
{
    std::vector<std::string> names;
    if (!g_ready || !mask) {
        return names;
    }

    Lock lock;
    const TypeIndexData& index = TypeIndex();
    for (const auto& [name, ids] : index.udts) {
        if (WildcardMatch(mask, name.c_str())) {
            names.push_back(name);
        }
    }
    for (const auto& name : index.enums) {
        if (WildcardMatch(mask, name.c_str())) {
            names.push_back("enum " + name);
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

bool Symbols_DescribeType(const char* typeName, std::vector<TypeMember>* members, uint64_t* size, int* otherCopies)
{
    members->clear();
    if (!g_ready || !typeName) {
        return false;
    }

    Lock lock;
    ULONG typeId = 0;
    if (!ChooseType(typeName, &typeId)) {
        return false;
    }

    ULONG64 length = 0;
    TypeInfo(typeId, TI_GET_LENGTH, &length);
    *size = length;
    *otherCopies = static_cast<int>(TypeCopies(typeName).size()) - 1;
    if (*otherCopies < 0) {
        *otherCopies = 0;
    }

    DWORD count = 0;
    if (!TypeInfo(typeId, TI_GET_CHILDRENCOUNT, &count) || count == 0) {
        return true;
    }

    std::vector<char> buf(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
    auto* params = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(buf.data());
    params->Count = count;
    params->Start = 0;
    if (!TypeInfo(typeId, TI_FINDCHILDREN, params)) {
        return true;
    }

    for (DWORD i = 0; i < count; ++i) {
        TypeMember m{};
        if (DescribeMember(params->ChildId[i], &m)) {
            members->push_back(m);
        }
    }
    std::stable_sort(members->begin(), members->end(),
                     [](const TypeMember& a, const TypeMember& b) { return a.offset < b.offset; });
    return true;
}

bool Symbols_EnumList(const char* enumType, std::vector<std::pair<std::string, int64_t>>* values)
{
    values->clear();
    if (!g_ready || !enumType) {
        return false;
    }

    Lock lock;
    ULONG typeId = 0;
    DWORD count = 0;
    if (!LookupType(enumType, &typeId) || !TypeInfo(typeId, TI_GET_CHILDRENCOUNT, &count)) {
        return false;
    }

    std::vector<char> buf(sizeof(TI_FINDCHILDREN_PARAMS) + count * sizeof(ULONG));
    auto* params = reinterpret_cast<TI_FINDCHILDREN_PARAMS*>(buf.data());
    params->Count = count;
    params->Start = 0;
    if (count && !TypeInfo(typeId, TI_FINDCHILDREN, params)) {
        return false;
    }

    for (DWORD i = 0; i < count; ++i) {
        WCHAR* name = nullptr;
        if (!TypeInfo(params->ChildId[i], TI_GET_SYMNAME, &name) || !name) {
            continue;
        }
        std::string s = Narrow(name);
        LocalFree(name);

        int64_t value = 0;
        VARIANT v = {};
        if (TypeInfo(params->ChildId[i], TI_GET_VALUE, &v)) {
            VariantInteger(v, &value); // anything else stays 0
        }
        values->push_back({s, value});
    }
    return true;
}
