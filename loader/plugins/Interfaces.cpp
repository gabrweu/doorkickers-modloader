// Interfaces (PublishInterface/GetInterface): function tables plugins share by name.
//
// Published only during init, looked up only after (PLUGINS_LOADED on), so lookups don't depend on load order. A
// switched-off publisher is withdrawn: its tables point into switched-off code. Doesn't touch the game (eventstest).
#include "Loader.h"

#include <algorithm>

namespace {

struct Published {
    std::string name;
    uint32_t version;
    const void* table;
    HMODULE owner;
    bool faulted;
    std::vector<HMODULE> users; // plugins that got it
};

// One lock: lookups come from any thread, switch-offs from any crash site. Nothing calls out under it.
SRWLOCK g_lock = SRWLOCK_INIT;
std::vector<Published> g_published;
std::vector<HMODULE> g_toldEarly; // lookup during init already logged
bool g_open = false;

struct Exclusive {
    Exclusive() { AcquireSRWLockExclusive(&g_lock); }

    ~Exclusive() { ReleaseSRWLockExclusive(&g_lock); }
};

Published* Find(const char* name)
{
    for (auto& p : g_published) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

void Interfaces_SetOpen(bool open)
{
    Exclusive lock;
    g_open = open;
}

const char* Interfaces_Check(const char* name, const void* table)
{
    if (!name || !*name) {
        return "no name";
    }

    size_t length = strnlen(name, kMaxInterfaceName + 1);
    if (length > kMaxInterfaceName) {
        return "name longer than 63 characters";
    }

    for (size_t i = 0; i < length; ++i) {
        if (name[i] <= ' ' || name[i] > '~') {
            return "name has characters other than printable ASCII (no spaces)";
        }
    }

    if (!table) {
        return "no table";
    }
    return nullptr;
}

DK2ML_Status Interfaces_Publish(HMODULE owner, const char* name, uint32_t version, const void* table)
{
    std::wstring who = Log_ModuleName(owner);
    if (const char* error = Interfaces_Check(name, table)) {
        LogF("%ls: PublishInterface ignored: %s", who.c_str(), error);
        return DK2ML_ERROR;
    }

    Exclusive lock;
    if (!g_open) {
        LogF("%ls: PublishInterface(\"%s\") after DK2ML_PluginInit returned, ignored (publish in init)", who.c_str(),
             name);
        return DK2ML_ERROR;
    }
    if (const Published* taken = Find(name)) {
        LogF("%ls: PublishInterface(\"%s\") ignored: %ls already published that name", who.c_str(), name,
             Log_ModuleName(taken->owner).c_str());
        return DK2ML_ERROR;
    }

    g_published.push_back({name, version, table, owner, false, {}});
    return DK2ML_OK;
}

const void* Interfaces_Get(HMODULE caller, const char* name, uint32_t minVersion, uint32_t* versionOut)
{
    if (versionOut) {
        *versionOut = 0;
    }
    if (!name) {
        return nullptr;
    }

    Exclusive lock;
    if (g_open) {
        if (std::find(g_toldEarly.begin(), g_toldEarly.end(), caller) == g_toldEarly.end()) {
            g_toldEarly.push_back(caller);
            LogF("%ls: GetInterface(\"%.63s\") during init returns NULL: look interfaces up from the PLUGINS_LOADED "
                 "event on",
                 Log_ModuleName(caller).c_str(), name);
        }
        return nullptr;
    }

    Published* p = Find(name);
    if (!p || p->faulted || p->version < minVersion) {
        return nullptr;
    }

    bool newUser = caller != p->owner && std::find(p->users.begin(), p->users.end(), caller) == p->users.end();
    if (newUser) {
        p->users.push_back(caller);
    }
    if (versionOut) {
        *versionOut = p->version;
    }
    return p->table;
}

void Interfaces_FaultOwner(HMODULE owner)
{
    Exclusive lock;
    for (auto& p : g_published) {
        if (p.owner == owner) {
            p.faulted = true;
        }
    }
}

void Interfaces_Of(HMODULE owner, std::vector<std::string>* published, std::vector<std::string>* used)
{
    Exclusive lock;
    for (const auto& p : g_published) {
        if (p.owner == owner) {
            published->push_back(p.name);
        }
        if (std::find(p.users.begin(), p.users.end(), owner) != p.users.end()) {
            used->push_back(p.name);
        }
    }
}

void Interfaces_Log()
{
    Exclusive lock;
    for (const auto& p : g_published) {
        LogF("interface \"%s\" v%u published by %ls%s", p.name.c_str(), p.version, Log_ModuleName(p.owner).c_str(),
             p.faulted ? " (switched off)" : "");
    }
}
