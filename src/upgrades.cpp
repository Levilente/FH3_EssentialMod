// The upgrade bridge (API 11, register_upgrade): custom upgrade types for Garage > Upgrades.
//
// A mod describes an upgrade (name, description, icons, stages with names, descriptions and prices). The platform hands
// out the ids, writes the database rows and texts, and does what the game has no code for: the game's shop only knows
// the part enums 0..42, so a type above that is routed through the game's own basket, purchase and "install this
// setup?" code by the hooks below. The per-car stage and the owned stages are kept in fh3-mods\platform-upgrades.ini.
//
// Ids of the upgrade in slot i (0..7): part enum 47+i, UpgradeTypes id 7452+i, Upgrades id 74520+10i+stage,
// part id 900470+10i+stage. (Slot 0 is what NitroSystem II always used, so its saves keep working.)
#include "platform.hpp"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace fh3platform {
namespace {

constexpr int kMaxUpgrades = 8;
constexpr int kMaxStages   = 10;
constexpr int kFirstEnum   = 47;
constexpr long kPartBase   = 900470;
constexpr unsigned long long kPreviewMs = 300;   // how long after a cursor move upgrade_stage answers with the previewed stage
constexpr uint32_t kUpgradesHash     = 0x968719E9u;
constexpr uint32_t kUpgradeTypesHash = 0x07710821u;

struct Stage {
    std::wstring name, desc;
    long price = 0;
    std::string icon, icon_lg;
};

struct Upgrade {
    FH3Mod* mod = nullptr;
    std::string id;
    int slot = 0;
    std::wstring name, desc;
    int area = 1;
    int camera_part = 0;   // a game part enum whose shop camera this upgrade uses (0 = default)
    std::string icon, icon_lg;
    std::vector<Stage> stages;
    FH3UpgradeStageFn cb = nullptr;
    void* user           = nullptr;
    std::atomic<long> pend_id{0};   // the stage chosen in the shop and not yet installed
    std::atomic<bool> pend{false};
    std::atomic<int> preview{-1};       // the stage the shop shows on the car right now (-1: none), for upgrade_stage
    std::atomic<long> preview_gid{0};
    std::atomic<unsigned long long> preview_until{0};   // GetTickCount64 until which the preview applies
    int penum() const { return kFirstEnum + slot; }
    long part(int stage) const { return kPartBase + 10L * slot + stage; }
    int count() const { return static_cast<int>(stages.size()); }
};

Upgrade* g_up[kMaxUpgrades] = {};
std::atomic<int> g_count{0};
std::mutex g_reg_mu;
std::atomic<bool> g_started{false};
std::atomic<unsigned> g_active{0};   // garage id of the car being driven
std::atomic<bool> g_foreign{false};  // the car being driven isn't one of the player's (a loaner): no custom upgrades
std::atomic<uint64_t> g_shop_tick{0};   // the last time the upgrade shop did something (its car is the player's)
std::atomic<uint64_t> g_expect_build{0}; // since when a new car of the player's is expected (car switch, shop)
std::atomic<long> g_cur_ord{0};      // car ordinal of the last UpgradeParts view

std::wstring ini_path() { return (azimuth_dir() / L"fh3-mods" / L"platform-upgrades.ini").wstring(); }

std::wstring wnum(long v) { return std::to_wstring(v); }
std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

bool readable(const void* p, size_t n) {
    if (reinterpret_cast<uintptr_t>(p) < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

Upgrade* by_enum(int e) {
    const int n = g_count.load();
    for (int i = 0; i < n; ++i)
        if (g_up[i]->penum() == e) return g_up[i];
    return nullptr;
}
Upgrade* by_part(long id, int* stage) {
    const int n = g_count.load();
    for (int i = 0; i < n; ++i) {
        const long s = id - g_up[i]->part(0);
        if (s >= 0 && s < g_up[i]->count()) {
            if (stage) *stage = static_cast<int>(s);
            return g_up[i];
        }
    }
    return nullptr;
}

// ---- per-car state -------------------------------------------------------------------------------------------
struct Car {
    int stage = 0;
    long owned = 1;   // bit n = stage n; stock is always owned
    long ord   = 0;   // the car model, so a reused garage id doesn't inherit the stage of a sold car
};
std::mutex g_car_mu;
std::map<std::pair<int, long>, Car> g_cars;

std::wstring car_section(const Upgrade* u) { return L"car." + wide(u->id); }

void save_car(const Upgrade* u, long gid, const Car& c) {
    const std::wstring v = wnum(c.stage) + L"|" + wnum(c.owned) + L"|" + wnum(c.ord);
    WritePrivateProfileStringW(car_section(u).c_str(), wnum(gid).c_str(), v.c_str(), ini_path().c_str());
}

Car& car_locked(const Upgrade* u, long gid) {
    const auto key = std::make_pair(u->slot, gid);
    auto it        = g_cars.find(key);
    if (it != g_cars.end()) return it->second;
    Car c;
    wchar_t buf[96] = L"";
    GetPrivateProfileStringW(car_section(u).c_str(), wnum(gid).c_str(), L"", buf, 96, ini_path().c_str());
    long a = 0, b = 1, d = 0;
    if (buf[0] && swscanf(buf, L"%ld|%ld|%ld", &a, &b, &d) >= 1) {
        c.stage = static_cast<int>(a < 0 ? 0 : (a >= u->count() ? u->count() - 1 : a));
        c.owned = b | 1;
        c.ord   = d;
    }
    return g_cars.emplace(key, c).first->second;
}

void notify(const Upgrade* u, long gid, int stage) {
    if (u->cb) u->cb(u->user, static_cast<uint32_t>(gid), stage);
}

int get_stage(const Upgrade* u, long gid, long ord) {
    if (gid <= 0 || gid >= 2000000000L) return 0;
    std::scoped_lock lk{g_car_mu};
    Car& c = car_locked(u, gid);
    if (ord > 0 && c.ord > 0 && c.ord != ord) {   // another model under that garage id: not this car's stage
        c = Car{};
        c.ord = ord;
        save_car(u, gid, c);
    }
    return c.stage;
}
long owned_mask(const Upgrade* u, long gid) {
    if (gid <= 0) return 1;
    std::scoped_lock lk{g_car_mu};
    return car_locked(u, gid).owned | 1;
}
bool is_owned(const Upgrade* u, long gid, int stage) { return stage == 0 || ((owned_mask(u, gid) >> stage) & 1); }

void set_stage(const Upgrade* u, long gid, long ord, int stage) {
    if (gid <= 0 || gid >= 2000000000L || stage < 0 || stage >= u->count()) return;
    {
        std::scoped_lock lk{g_car_mu};
        Car& c = car_locked(u, gid);
        c.stage = stage;
        c.owned |= 1L << stage;
        if (ord > 0) c.ord = ord;
        save_car(u, gid, c);
    }
    if (gid == static_cast<long>(g_active.load())) notify(u, gid, stage);
}

void clear_previews() {
    const int n = g_count.load();
    for (int i = 0; i < n; ++i) g_up[i]->preview.store(-1);
}

void activate(unsigned gid, const char* why = "") {
    if (gid == 0 || gid >= 2000000000u) return;
    if (gid == g_active.load()) {
        if (g_foreign.exchange(false)) {   // back in the player's own car
            plog("upgrades: driven car is garage " + std::to_string(gid) + " again (" + why + ")");
            const int n = g_count.load();
            for (int i = 0; i < n; ++i) notify(g_up[i], gid, get_stage(g_up[i], gid, 0));
        }
        return;
    }
    g_foreign.store(false);
    g_expect_build.store(GetTickCount64());
    clear_previews();
    for (int i = 0; i < g_count.load(); ++i) g_up[i]->pend.store(false);   // a basket belongs to the car it was filled for
    plog("upgrades: driven car is garage " + std::to_string(gid) + " (was " + std::to_string(g_active.load()) + ", " + why + ")");
    g_active.store(gid);
    WritePrivateProfileStringW(L"general", L"LastGarage", wnum(gid).c_str(), ini_path().c_str());
    const int n = g_count.load();
    for (int i = 0; i < n; ++i) notify(g_up[i], gid, get_stage(g_up[i], gid, 0));
}

// ---- the database rows ------------------------------------------------------------------------------------------
std::string sq(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '\'') o += '\'';
        o += c;
    }
    return o;
}
std::string ref(uint32_t table, uint32_t key) { return "_&" + std::to_string((static_cast<unsigned long long>(table) << 32) | key); }
uint32_t key_base(const Upgrade* u) { return 0x4E530000u + static_cast<uint32_t>(u->slot) * 0x100u; }

const char kIcons[] = "GAME:\\Media\\UI\\Textures\\Data_Bound\\Upgrade_Parts\\";

std::string build_sql(const Upgrade* u) {
    std::string out;
    char b[1400];
    const std::string dflt    = std::string(kIcons) + "Engine_Power\\engine_power_Icon.swatchbin";
    const std::string dflt_lg = std::string(kIcons) + "Engine_Power\\engine_power_LG_Icon.swatchbin";
    const std::string stock   = std::string(kIcons) + "stock_Icon.swatchbin";
    const std::string stock_lg = std::string(kIcons) + "stock_LG_Icon.swatchbin";
    const int type_id = 7452 + u->slot;
    snprintf(b, sizeof b,
             "INSERT OR REPLACE INTO UpgradeTypes (id, PartEnum, IsException, DisplayOrder, EverymanUpgradeDisplayOrder, Name, Description, IconPath, ImagePath) VALUES (%d, %d, 0, %d, -1, '%s', '%s', '%s', '%s');\n",
             type_id, u->penum(), 15 + u->slot, ref(kUpgradeTypesHash, key_base(u) + 0x10).c_str(), ref(kUpgradeTypesHash, key_base(u) + 0x11).c_str(),
             sq(u->icon.empty() ? dflt : u->icon).c_str(), sq(u->icon_lg.empty() ? dflt_lg : u->icon_lg).c_str());
    out += b;
    snprintf(b, sizeof b, "INSERT OR REPLACE INTO UpgradeAreaForUpgradeType (id, UpgradeAreaId, UpgradeTypeId) VALUES (%d, %d, %d);\n", type_id, u->area, type_id);
    out += b;
    for (int l = 0; l < u->count(); ++l) {
        const Stage& s = u->stages[l];
        const std::string i1 = !s.icon.empty() ? s.icon : (l ? dflt : stock);
        const std::string i2 = !s.icon_lg.empty() ? s.icon_lg : (l ? dflt_lg : stock_lg);
        snprintf(b, sizeof b,
                 "INSERT OR REPLACE INTO Upgrades (id, TypeId, IsException, Name, Level, Description, IconPath, ImagePath) VALUES (%d, '%d', 0, '%s', %d, '%s', '%s', '%s');\n",
                 74520 + 10 * u->slot + l, type_id, ref(kUpgradesHash, key_base(u) + 0x20 + l).c_str(), l, ref(kUpgradesHash, key_base(u) + 0x30 + l).c_str(), sq(i1).c_str(), sq(i2).c_str());
        out += b;
    }
    return out;
}

// ---- statements ---------------------------------------------------------------------------------------------------
bool starts(const char* z, size_t len, const char* p) {
    const size_t n = strlen(p);
    return len >= n && strncmp(z, p, n) == 0;
}
long num_at(const char* p) {
    while (*p == ' ') ++p;
    return atol(p);
}
// "KEY<spaces>=<spaces>N" -> N (found = true)
bool value_of(const char* z, const char* key, long* v) {
    const char* p = strstr(z, key);
    if (!p) return false;
    p += strlen(key);
    while (*p == ' ') ++p;
    if (*p != '=') return false;
    ++p;
    *v = num_at(p);
    return true;
}

std::atomic<uint64_t> g_kit_tick{0};

// Exec through the game's own sqlite3_exec.
typedef int (*ExecFn)(void*, const char*, void*, void*, char**);
thread_local bool t_in_repair = false;
std::atomic<bool> g_repaired{false};
std::atomic<int> g_repair_tries{0};

char* game() { return static_cast<char*>(find_game()); }

// The game forgets a part when another one of the type is installed; our stages stay owned (stock always).
void ensure_owned(void* db, const Upgrade* u, long gid) {
    if (gid <= 0 || t_in_repair || !game()) return;
    long mask = owned_mask(u, gid) | 1L;
    mask |= 1L << get_stage(u, gid, 0);
    std::string q;
    for (int l = 0; l < u->count(); ++l)
        if (mask & (1L << l)) {
            char b[460];
            snprintf(b, sizeof b,
                     "INSERT INTO Profile0_Career_PurchasedParts (GarageId, PartEnum, PartId, ManufacturerId, PricePaid) SELECT %ld, %d, %ld, -1, 0 WHERE NOT EXISTS (SELECT 1 FROM Profile0_Career_PurchasedParts WHERE GarageId = %ld AND PartEnum = %d AND PartId = %ld);",
                     gid, u->penum(), u->part(l), gid, u->penum(), u->part(l));
            q += b;
        }
    t_in_repair = true;
    const int r = reinterpret_cast<ExecFn>(game() + 0x421D0)(db, q.c_str(), nullptr, nullptr, nullptr);
    t_in_repair = false;
    static std::atomic<int> lg{0};
    if (lg.fetch_add(1) < 4) plog("upgrades: owned rows for garage " + std::to_string(gid) + " (" + u->id + ", mask " + std::to_string(mask) + "), exec " + std::to_string(r));
}

// A save that still holds one of our ids in a part column crashes the game on load (no real part behind it).
void repair_save(void* db, const char* z) {
    if (g_repaired.load() || t_in_repair || !z || !game() || !strstr(z, "Profile0_Career_Garage")) return;
    if (g_repair_tries.fetch_add(1) >= 6) return;
    t_in_repair = true;
    ExecFn ex = reinterpret_cast<ExecFn>(game() + 0x421D0);
    const int r = ex(db, "UPDATE Profile0_Career_Garage SET QuadTurbo=-1, AspirationTypeId=1 WHERE QuadTurbo BETWEEN 900470 AND 900569", nullptr, nullptr, nullptr);
    ex(db, "UPDATE Profile0_Career_Garage SET MotorParts=-1 WHERE MotorParts BETWEEN 900470 AND 900569", nullptr, nullptr, nullptr);
    ex(db, "DELETE FROM Profile0_Career_PurchasedParts WHERE PartEnum IN (26, 42) AND PartId BETWEEN 900470 AND 900569", nullptr, nullptr, nullptr);
    if (r == 0) ex(db, "UPDATE Profile0_Career_Garage SET AspirationTypeId=1 WHERE AspirationTypeId=4 AND QuadTurbo<0", nullptr, nullptr, nullptr);
    t_in_repair = false;
    plog("upgrades: save clean-up returned " + std::to_string(r));
    if (r == 0) g_repaired.store(true);
}

// ---- which of the player's cars is which model -----------------------------------------------------------------
// The driven garage id is learned from the game's statements (stats saved, shop queries), which can come late: a
// car chosen at an event's car select is built and driven a few seconds before its stats are saved. The car itself
// knows its Data_Car id, so the platform keeps the player's garage (garage id -> CarId) and the time the game last
// read each garage car, and upgrades_garage_for_car() answers "which of the player's cars is this?".
std::mutex g_gar_mu;
std::unordered_map<long, long> g_gar_car;         // garage id -> Data_Car id (CarId)
std::unordered_map<long, uint64_t> g_gar_touch;   // garage id -> GetTickCount64 of the game's last statement on it
std::atomic<uint64_t> g_gar_loaded{0};
std::atomic<bool> g_gar_dirty{true};

long car_model(long v) { return v >= 100000 ? v / 1000 : v; }   // Data_Car id 2154003 -> model 2154; 445 stays 445

int gar_row(void* p, int n, char** v, char**) {
    if (n >= 2 && v && v[0] && v[1]) static_cast<std::unordered_map<long, long>*>(p)->emplace(atol(v[0]), atol(v[1]));
    return 0;
}

void refresh_garage_map(void* db) {
    const uint64_t now = GetTickCount64();
    if (!db || t_in_repair || !game() || (!g_gar_dirty.load() && now - g_gar_loaded.load() < 15000)) return;
    std::unordered_map<long, long> m;
    t_in_repair = true;
    const int r = reinterpret_cast<ExecFn>(game() + 0x421D0)(db, "SELECT Id, CarId FROM Profile0_Career_Garage", reinterpret_cast<void*>(&gar_row), &m, nullptr);
    t_in_repair = false;
    g_gar_loaded.store(now);
    if (r != 0) return;
    g_gar_dirty.store(false);
    size_t before = 0;
    {
        std::scoped_lock lk{g_gar_mu};
        before = g_gar_car.size();
        g_gar_car.swap(m);
    }
    if (before != g_gar_car.size()) plog("upgrades: the player's garage has " + std::to_string(g_gar_car.size()) + " cars");
}

// Only the statements the game runs when it puts the player into a garage car count ("... AND IsCarInstalled(Car)",
// "SELECT * ... WHERE Id=n"): browsing the garage reads every car with "SELECT 1 ... WHERE Id=n", which says nothing
// about the car that will be driven.
void touch_garage(long gid, const char* z) {
    if (gid <= 0 || gid >= 2000000000L) return;
    if (!strstr(z, "IsCarInstalled") && strncmp(z, "SELECT * FROM Profile0_Career_Garage WHERE Id", 45) != 0) return;
    {
        std::scoped_lock lk{g_gar_mu};
        g_gar_touch[gid] = GetTickCount64();
    }
    static std::atomic<long> last{0};
    static std::atomic<int> lines{0};
    if (last.exchange(gid) != gid && lines.fetch_add(1) < 120) plog("upgrades: the game reads garage car " + std::to_string(gid) + " (" + std::string(z, strnlen(z, 70)) + ")");
}

// The game's statements on the player's garage: note the garage id they name; refresh the map now and then.
void watch_garage(void* db, const char* z, size_t len) {
    if (len < 20 || !strstr(z, "Profile0_")) return;
    const bool garage_table = strstr(z, "Profile0_Career_Garage") != nullptr;
    if (garage_table && (z[0] == 'I' || z[0] == 'D' || z[0] == 'R')) {   // INSERT / DELETE / REPLACE: a car bought or sold
        g_gar_dirty.store(true);
        return;
    }
    long v = 0;
    if (value_of(z, "GarageId", &v) || value_of(z, "GarageID", &v) || (garage_table && value_of(z, " WHERE Id", &v))) touch_garage(v, z);
    if (garage_table) refresh_garage_map(db);
}

// The game's statements for a part above enum 42 name the table entry every such enum shares (MotorParts).
bool rewrite(const char* z, size_t len, std::string& out) {
    if (len < 30) return false;
    // price / manufacturer / level of a part of ours
    if (z[0] == 'S') {
        static const char mf[] = "SELECT ManufacturerID FROM List_UpgradeMotorParts WHERE Id=";
        static const char pr[] = "SELECT Price FROM List_UpgradeMotorParts WHERE Id = ";
        static const char lv[] = "SELECT Level FROM List_UpgradeMotorParts WHERE ";
        int st = 0;
        if (starts(z, len, mf)) {
            if (by_part(num_at(z + sizeof mf - 1), &st)) { out = "SELECT -1 AS ManufacturerID"; return true; }
            return false;
        }
        if (starts(z, len, pr)) {
            if (Upgrade* u = by_part(num_at(z + sizeof pr - 1), &st)) { out = "SELECT " + std::to_string(st ? u->stages[st].price : 0L) + " AS Price"; return true; }
            return false;
        }
        if (starts(z, len, lv)) {
            long id = 0;
            if (value_of(z, " AND Id", &id) && by_part(id, &st)) { out = "SELECT " + std::to_string(st) + " AS Level"; return true; }
            return false;
        }
        // the part list of our type: the installed row points at the car's stage
        if (starts(z, len, "SELECT PartTable.PartId AS id")) {
            long e = 0;
            if (!value_of(z, "WHERE PartTable.PartEnum", &e)) return false;
            Upgrade* u = by_enum(static_cast<int>(e));
            if (!u) return false;
            long gid = -1;
            if (const char* g = strstr(z, "purchasedParts.GarageId = ")) gid = atol(g + 26);
            static const char K[] = "WHEN PartTable.PartId = ";
            const char* k         = strstr(z, K);
            if (!k) return false;
            const char* num = k + sizeof K - 1;
            if (*num == '-') ++num;
            const char* after = num;
            while (*after >= '0' && *after <= '9') ++after;
            if (after == num || strncmp(after, " THEN '1'", 9) != 0) return false;
            out.assign(z, static_cast<size_t>(k - z) + sizeof K - 1);
            out += std::to_string(u->part(get_stage(u, gid, g_cur_ord.load())));
            out.append(after, len - static_cast<size_t>(after - z));
            return true;
        }
        return false;
    }
    if (z[0] == 'C') {   // CREATE VIEW ...View_UpgradeParts: our rows for the car the view is built for
        if (!starts(z, len, "CREATE VIEW") || !strstr(z, "View_UpgradeParts AS SELECT 0 AS PartEnum") || strstr(z, "900470 AS PartId")) return false;
        const char* w = strstr(z, "WHERE Ordinal = ");
        if (!w) return false;
        const long ord = atol(w + 16);
        const char* un = strstr(z, " UNION SELECT ");
        if (!ord || !un || un < w) return false;
        out.assign(z, static_cast<size_t>(un - z));
        const int n = g_count.load();
        for (int i = 0; i < n; ++i)
            for (int l = 0; l < g_up[i]->count(); ++l)
                out += " UNION SELECT " + std::to_string(g_up[i]->penum()) + " AS PartEnum, " + std::to_string(g_up[i]->part(l)) + " AS PartId, " + std::to_string(ord) +
                       " AS Ordinal, " + std::to_string(l == 0) + " AS IsStock, " + std::to_string(l) + " AS Level, NULL AS ManufacturerID, " +
                       std::to_string(l ? g_up[i]->stages[l].price : 0L) + " AS Price, 0 AS IsException";
        out.append(un, len - static_cast<size_t>(un - z));
        g_cur_ord.store(ord);
        g_shop_tick.store(GetTickCount64());
        g_expect_build.store(GetTickCount64());
        return true;
    }
    if (z[0] == 'U') {
        if (!starts(z, len, "UPDATE Profile0_Career_Garage SET")) return false;
        if (len > 40 && starts(z, len, "UPDATE Profile0_Career_Garage SET AspirationTypeId=4 WHERE Id=") && GetTickCount64() - g_kit_tick.load() < 5000) {
            out = std::string("UPDATE Profile0_Career_Garage SET AspirationTypeId=AspirationTypeId") + strstr(z, " WHERE Id=");
            return true;
        }
        long id = 0;
        int st  = 0;
        if (!value_of(z, "MotorParts", &id)) return false;
        Upgrade* u = by_part(id, &st);
        if (!u) return false;
        g_kit_tick.store(GetTickCount64());
        const char* w = strstr(z, " WHERE Id=");
        const long gid = w ? atol(w + 10) : 0;
        u->pend.store(false);
        if (gid > 0) {
            set_stage(u, gid, g_cur_ord.load(), st);
            activate(static_cast<unsigned>(gid), "stage installed");
        }
        // The game writes its whole idea of the car's parts (our id in the column and whatever it clears because of it).
        // An upgrade of ours is no engine part: nothing of that may reach the car; only the stage is kept, above.
        out = "UPDATE Profile0_Career_Garage SET MotorParts=MotorParts WHERE Id=" + std::to_string(gid);
        plog("upgrades: " + u->id + " stage " + std::to_string(st) + " installed on garage " + std::to_string(gid));
        return true;
    }
    return false;
}

typedef int (*PrepareFn)(void*, const char*, int, unsigned, void*, void**, const char**);
PrepareFn g_orig_prepare = nullptr;

std::atomic<void*> g_live_db{nullptr};   // the connection the game keeps the profile in (seen on its Profile0_ statements)

int detour_prepare(void* db, const char* z, int n, unsigned fl, void* old, void** pp, const char** tail) {
    if (db && z && readable(z, 24) && strncmp(z, "UPDATE Profile0_", 16) == 0) g_live_db.store(db);
    if (!t_in_repair && !g_repaired.load()) repair_save(db, z);
    if (z && readable(z, 64) && g_count.load() > 0) {
        const size_t len = n < 0 ? strlen(z) : static_cast<size_t>(n);
        if (!t_in_repair) watch_garage(db, z, len);
        if (len > 24 && z[0] == 'U' && strncmp(z, "UPDATE Profile0_Career_Garage SET TopSpeed", 42) == 0) {
            const char* w = strstr(z, " WHERE Id=");   // the driven car's stats are saved: that is its garage id
            if (w) activate(static_cast<unsigned>(atol(w + 10)), "car stats saved");
        }
        if (len > 60 && z[0] == 'S' && !t_in_repair && starts(z, len, "SELECT 1 From Profile0_Career_PurchasedParts WHERE GarageId = ")) {
            long e = 0;
            const char* pe = strstr(z, " AND PartEnum");
            if (pe && value_of(pe, " AND PartEnum", &e))
                if (Upgrade* u = by_enum(static_cast<int>(e))) ensure_owned(db, u, atol(z + 62));
        } else if (z[0] == 'S' && starts(z, len, "SELECT PartTable.PartId AS id")) {
            long e = 0;
            if (value_of(z, "WHERE PartTable.PartEnum", &e))
                if (Upgrade* u = by_enum(static_cast<int>(e))) {
                    const char* g = strstr(z, "purchasedParts.GarageId = ");
                    if (g) ensure_owned(db, u, atol(g + 26));
                }
        }
        std::string q;
        if (!t_in_repair && rewrite(z, len, q)) {
            if (z[0] == 'U' || z[0] == 'S') {
                const char* g = strstr(z, "GarageId = ");
                if (z[0] == 'S' && g) activate(static_cast<unsigned>(atol(g + 11)), "query on that garage id");
            }
            const int r = g_orig_prepare(db, q.c_str(), -1, fl, old, pp, tail);
            if (tail && readable(tail, sizeof *tail)) *tail = z + len;   // some callers (sqlite3_exec chain) pass a non-pointer here
            return r;
        }
    }
    return g_orig_prepare(db, z, n, fl, old, pp, tail);
}

// ---- the shop ----------------------------------------------------------------------------------------------------
typedef void* (*Fn12)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
#define ARGS12 uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h, uint64_t i, uint64_t j, uint64_t k, uint64_t l
#define PASS12 a, b, c, d, e, f, g, h, i, j, k, l

Fn12 g_orig_select = nullptr, g_orig_add = nullptr, g_orig_commit = nullptr, g_orig_abandon = nullptr, g_orig_afford = nullptr;
typedef int (*MemcmpFn)(const void*, const void*, size_t);
typedef int (*DiffFn)(const void*, const void*);
typedef void* (*QueryBuildA)(void*, void*, int);
typedef void* (*QueryBuildB)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
MemcmpFn g_orig_memcmp = nullptr;
DiffFn g_orig_diff     = nullptr;
QueryBuildA g_orig_qa  = nullptr;
QueryBuildB g_orig_qb  = nullptr;
thread_local bool t_sel_new = false;

bool any_pending() {
    const int n = g_count.load();
    for (int i = 0; i < n; ++i)
        if (g_up[i]->pend.load()) return true;
    return false;
}

// The A handler (0x16DC050) first compares the car's part set (0xB4 bytes = the 43 enum slots) with the one being
// previewed and leaves when they are equal. A part of an enum above 42 never changes those bytes, so it never counts
// as a change. While that handler runs for such a part, that one comparison says "different".
void* detour_select(ARGS12) {
    g_shop_tick.store(GetTickCount64());
    g_expect_build.store(GetTickCount64());
    const bool ours = static_cast<int>(b) > 42;
    if (ours) t_sel_new = true;
    void* r = g_orig_select(PASS12);
    t_sel_new = false;
    return r;
}

int detour_memcmp(const void* x, const void* y, size_t n) {
    // Leaving the upgrade shop (0x16FABA0) asks "Modified car - install this setup?" only when the car's setup
    // (0x16C bytes) differs from the one it came in with; a stage chosen in the shop counts as a difference too.
    if (n == 0x16C && game() && reinterpret_cast<uintptr_t>(__builtin_return_address(0)) == reinterpret_cast<uintptr_t>(game()) + 0x16FAC40) {
        const int r = g_orig_memcmp(x, y, n);
        return r ? r : (any_pending() ? 1 : 0);
    }
    if (t_sel_new && n == 0xB4) {
        t_sel_new = false;
        return 1;
    }
    if (n == 0xB4 && GetTickCount64() - g_shop_tick.load() < 15000) {   // DIAGNOSTIC (temporary): who compares part sets in the shop
        static std::atomic<int> lines{0};
        const int r = g_orig_memcmp(x, y, n);
        if (lines.fetch_add(1) < 300) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
            const uintptr_t gb  = reinterpret_cast<uintptr_t>(game());
            int pv = -1;
            for (int q = 0; q < g_count.load(); ++q) pv = std::max(pv, g_up[q]->preview.load());
            char buf[160];
            snprintf(buf, sizeof buf, "upgrades: DIAG part sets compared (caller game+0x%llX): %s, preview %d", static_cast<unsigned long long>(gb && ret > gb ? ret - gb : 0), r ? "different" : "equal", pv);
            plog(buf);
        }
        return r;
    }
    return g_orig_memcmp(x, y, n);
}

// The shop's "show this part on the car" step (0x38D0B0: car, enum, part id, ..., refresh flag). It puts the part in
// the car's part set (our enums are kept out of it by the part setter below) and, with the refresh flag (the shop's
// preview), rebuilds the car. For a stage of ours it records that stage as the preview, so upgrade_stage answers
// with it while the car is rebuilt: a mod that builds the car from upgrade_stage then shows the stage on hover.
QueryBuildB g_orig_show = nullptr;
std::atomic<int> g_show_logged{0};
void* detour_show(uint64_t car, uint64_t e, uint64_t part, uint64_t d, uint64_t flag, uint64_t g, uint64_t h, uint64_t i, uint64_t j) {
    g_shop_tick.store(GetTickCount64());
    g_expect_build.store(GetTickCount64());
    const bool shop = (flag & 0xFF) != 0;
    const int en    = static_cast<int>(e);
    if (shop) {
        if (en > 43) {
            // Each upgrade of ours has its own enum. A part id that isn't one of its stages is the game putting back
            // what the car had (it reads that from past the part set, where our parts never are): the preview ends.
            const int n = g_count.load();
            for (int q = 0; q < n; ++q) {
                Upgrade* u = g_up[q];
                if (u->penum() != en) continue;
                const long s = static_cast<long>(static_cast<int>(part)) - u->part(0);
                if (s >= 0 && s < u->count()) {
                    u->preview_gid.store(static_cast<long>(g_active.load()));
                    u->preview_until.store(GetTickCount64() + kPreviewMs);
                    u->preview.store(static_cast<int>(s));
                } else {
                    u->preview.store(-1);
                }
                if (g_show_logged.fetch_add(1) < 300)
                    plog("upgrades: " + u->id + (s >= 0 && s < u->count() ? " stage " + std::to_string(s) + " shown in the shop (preview)"
                                                                       : " back to the fitted stage (part " + std::to_string(static_cast<int>(part)) + ")"));
            }
        } else {
            // The cursor is on one of the game's own parts: none of ours is on screen any more.
            clear_previews();
        }
    }
    return g_orig_show(car, e, part, d, flag, g, h, i, j);
}

// Car rebuilds (0x3939F0, called by the show step with refresh on): logged with the caller, to find the shop's
// other ways of putting the car back (diagnostics, capped).
QueryBuildA g_orig_rebuild = nullptr;
std::atomic<int> g_rebuild_logged{0};
void* detour_rebuild(void* car, void* x, int y) {
    if (g_rebuild_logged.fetch_add(1) < 300) {
        const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
        const uintptr_t gb  = reinterpret_cast<uintptr_t>(game());
        int pv = -1;
        const int n = g_count.load();
        for (int q = 0; q < n; ++q) pv = std::max(pv, g_up[q]->preview.load());
        char buf[160];
        snprintf(buf, sizeof buf, "upgrades: car rebuilt (caller game+0x%llX, preview %s)", static_cast<unsigned long long>(gb && ret > gb ? ret - gb : 0),
                 pv >= 0 ? "on" : "off");
        plog(buf);
    }
    return g_orig_rebuild(car, x, y);
}

// The shop puts a whole setup (part set + tuning) on its car with 0x38BCD0 (car, part set, r8b, r9b, ...): when it
// enters or leaves a part list, after a basket add, and before each cursor move (0x16F98E8, right before the show
// step). Our stages are never in that set, so whatever it puts on the car isn't the stage on screen any more: the
// preview ends here, and the show step that may follow sets it again. Without this the previewed stage stayed on
// the car after backing out of the list, and even after leaving the garage.
QueryBuildB g_orig_setup = nullptr;
std::atomic<int> g_setup_logged{0};
void* detour_setup(uint64_t car, uint64_t set, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h, uint64_t i) {
    g_shop_tick.store(GetTickCount64());
    g_expect_build.store(GetTickCount64());
    bool had = false;
    const int n = g_count.load();
    for (int q = 0; q < n; ++q) had |= g_up[q]->preview.load() >= 0;
    {   // DIAGNOSTIC (temporary)
        static std::atomic<int> lines{0};
        if (!had && lines.fetch_add(1) < 200) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
            const uintptr_t gb  = reinterpret_cast<uintptr_t>(game());
            char buf[120];
            snprintf(buf, sizeof buf, "upgrades: DIAG setup on the car (caller game+0x%llX), no preview", static_cast<unsigned long long>(gb && ret > gb ? ret - gb : 0));
            plog(buf);
        }
    }
    if (had) {
        clear_previews();
        if (g_setup_logged.fetch_add(1) < 200) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
            const uintptr_t gb  = reinterpret_cast<uintptr_t>(game());
            char buf[160];
            snprintf(buf, sizeof buf, "upgrades: the shop put a setup on the car (caller game+0x%llX): preview ended",
                     static_cast<unsigned long long>(gb && ret > gb ? ret - gb : 0));
            plog(buf);
        }
    }
    return g_orig_setup(car, set, c, d, e, f, g, h, i);
}

// The shop's camera for a part list (0x16F7ED0: this, edx = upgrade area, r8d = part enum) builds the camera preset's
// name ("Upgrades_WheelStyle_Front" ...) from the enum; an enum of ours has none (and the name lookup 0x19D67B0 reads past
// its table). An upgrade that asks for the camera of a game part gets that part's enum here.
QueryBuildB g_orig_camera = nullptr;
void* detour_camera(uint64_t self, uint64_t area, uint64_t e, uint64_t d, uint64_t f, uint64_t g, uint64_t h, uint64_t i, uint64_t j) {
    g_shop_tick.store(GetTickCount64());
    g_expect_build.store(GetTickCount64());
    const int en = static_cast<int>(e);
    if (en > 43) {
        const int n = g_count.load();
        for (int q = 0; q < n; ++q)
            if (g_up[q]->penum() == en && g_up[q]->camera_part > 0) {
                e = static_cast<uint32_t>(g_up[q]->camera_part);
                break;
            }
    }
    return g_orig_camera(self, area, e, d, f, g, h, i, j);
}

// The game's part setter (0x36B7A0: enum, part id, ..., part set, change list) stores the part at set + 8 + 4*enum
// without a bounds check. The part set is 0xB4 bytes (enum 43 lives at +4, 0..42 at +8) and the car's tuning setup
// follows it directly, so a part of enum 47+ written there overwrote tuning values: NitroSystem's kit (enum 47) hit
// the brake balance and the spacers (48/49) took the handbrake away until the next start. Our slots live in
// platform-upgrades.ini, so the game never needs them in its part set: such a call is dropped.
typedef void (*PartSetFn)(int, int, int, int, void*, void*);
PartSetFn g_orig_partset = nullptr;
std::atomic<int> g_partset_logged{0};
void detour_partset(int e, int part, int c, int d, void* set, void* changes) {
    if (e > 43) {
        if (g_partset_logged.fetch_add(1) < 40) {
            const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
            const uintptr_t gb  = reinterpret_cast<uintptr_t>(game());
            char buf[160];
            snprintf(buf, sizeof buf, "upgrades: kept part %d (enum %d) out of the game's part set (caller game+0x%llX)", part, e,
                     static_cast<unsigned long long>(gb && ret > gb ? ret - gb : 0));
            plog(buf);
        }
        return;
    }
    g_orig_partset(e, part, c, d, set, changes);
}

// Leaving the shop asks "buy the basket?" only when the car's part set (slots 0..42) differs from the previewed one.
int detour_diff(const void* a, const void* b) {
    const int r = g_orig_diff(a, b);
    return (r & 0xFF) ? r : (any_pending() ? 1 : r);
}

// The second stop in the A handler's worker (0x16FAEF0): is the previewed part different from the installed one is
// read from the 43-slot part sets, which for a new slot is memory past the set. The branch at 0x16FB341 is routed
// through this stub, which says "different" for such a part.
}  // namespace
extern "C" uintptr_t g_fh3_skip_target;
uintptr_t g_fh3_skip_target = 0;
extern "C" void fh3_basket_stub();
__asm__(".text\n.globl fh3_basket_stub\nfh3_basket_stub:\n"
        "test %al,%al\n"
        "jnz 1f\n"
        "push %rdx\n"
        "mov 0x48(%rbp),%rdx\n"
        "mov 0x5f0(%rdx),%edx\n"
        "cmp $42,%edx\n"
        "pop %rdx\n"
        "jg 1f\n"
        "add $8,%rsp\n"
        "jmp *g_fh3_skip_target(%rip)\n"
        "1: ret\n");
// the player's car, from the HUD controller (defined below the shop code)
void* detour_carbuild(ARGS12);
extern Fn12 g_orig_carbuild;
uint64_t detour_hud(void* c, void* a, void* b, void* d);
typedef uint64_t (*HudCtrlFn)(void*, void*, void*, void*);
HudCtrlFn g_orig_hud = nullptr;

namespace {

void install_basket_patch(uintptr_t base) {
    unsigned char* site = reinterpret_cast<unsigned char*>(base + 0x16FB341);
    unsigned char* slot = reinterpret_cast<unsigned char*>(base + 0x16FB5E8);
    static const unsigned char orig[6] = {0x0F, 0x84, 0x0D, 0x02, 0x00, 0x00};
    if (memcmp(site, orig, 6) != 0) { plog("upgrades: basket patch: unexpected code at the branch, not patched"); return; }
    for (int i = 0; i < 8; ++i)
        if (slot[i] != 0xCC) { plog("upgrades: basket patch: slot area is not free, not patched"); return; }
    g_fh3_skip_target = base + 0x16FB554;
    DWORD oldp;
    if (!VirtualProtect(slot, 8, PAGE_EXECUTE_READWRITE, &oldp)) return;
    *reinterpret_cast<uintptr_t*>(slot) = reinterpret_cast<uintptr_t>(&fh3_basket_stub);
    VirtualProtect(slot, 8, oldp, &oldp);
    const int64_t rel = static_cast<int64_t>(reinterpret_cast<uintptr_t>(slot)) - static_cast<int64_t>(reinterpret_cast<uintptr_t>(site + 6));
    if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldp)) return;
    site[0] = 0xFF;
    site[1] = 0x15;
    *reinterpret_cast<int32_t*>(site + 2) = static_cast<int32_t>(rel);
    VirtualProtect(site, 6, oldp, &oldp);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    plog("upgrades: basket patch installed (a stage of a new slot counts as a change in the add step)");
}

// A stage chosen in the basket: owned stages install with the setup, others wait for the commit.
void* detour_add(ARGS12) {
    if (static_cast<int>(c) > 42) {
        int st = 0;
        if (Upgrade* u = by_part(static_cast<int>(b), &st); u && u->penum() == static_cast<int>(c)) {
            const long gid = g_active.load();
            u->pend_id.store(static_cast<long>(static_cast<int>(b)));
            u->pend.store(st != get_stage(u, gid, 0));   // choosing the stage the car has is no change
            static std::atomic<int> add_logged{0};
            if (add_logged.fetch_add(1) < 200)
                plog("upgrades: basket add of " + u->id + " stage " + std::to_string(st) + " (fitted " + std::to_string(get_stage(u, gid, 0)) +
                     ", " + (is_owned(u, gid, st) ? "owned" : "not owned") + ", waiting " + (u->pend.load() ? "yes" : "no") + ")");
            // Owned stages go into the game's basket like any owned part: at 0 CR, replacing what was waiting for that slot
            // (the game's rows say they are owned, see ensure_owned), and the commit below fits them without a charge.
        }
    }
    return g_orig_add(PASS12);
}

// The shop's commit (0x4081A0) runs the game's purchase routine for the slots 0..42 only. A waiting stage is bought
// here with the pieces of that routine (credits getter, the PurchasedParts row, credits setter).
unsigned g_fault_at = 0;
int buy_filter(EXCEPTION_POINTERS* ep) {
    g_fault_at = static_cast<unsigned>(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress) - reinterpret_cast<uintptr_t>(game()));
    return EXCEPTION_EXECUTE_HANDLER;
}
unsigned g_last_credits = 0;
int buy_raw(void* sess, int gid, const Upgrade* u, int stage) {
    char* gb   = game();
    void* acct = *reinterpret_cast<void**>(static_cast<char*>(sess) + 8);
    if (!acct) return -2;
    alignas(16) unsigned char tmp[0x100];
    memset(tmp, 0, sizeof tmp);
    void* v = reinterpret_cast<void* (*)(void*, void*, const void*)>(gb + 0x323A00)(acct, tmp, gb + 0x2D39008);
    const unsigned credits = reinterpret_cast<unsigned (*)(void*)>(gb + 0x1A2FDA0)(v);
    reinterpret_cast<void (*)(void*)>(gb + 0x1A2DFF0)(tmp);
    const long price = u->stages[stage].price;
    g_last_credits = credits;
    if (credits < static_cast<unsigned>(price)) return -1;
    reinterpret_cast<void (*)(void*, int, int, int, int, int)>(gb + 0x36BDA0)(sess, gid, u->penum(), static_cast<int>(u->part(stage)), -1, static_cast<int>(price));
    reinterpret_cast<void (*)(void*, unsigned, void*)>(gb + 0x337440)(acct, credits - static_cast<unsigned>(price), nullptr);
    return 1;
}
int buy_guarded(void* sess, int gid, const Upgrade* u, int stage) {
    int r = 0;
    __try {
        r = buy_raw(sess, gid, u, stage);
    } __except (buy_filter(GetExceptionInformation())) {
        r = -3;
    }
    plog("upgrades: purchase of " + u->id + " stage " + std::to_string(stage) + " for " + std::to_string(u->stages[stage].price) + ": " +
         (r == 1 ? "paid" : r == -1 ? "not enough credits" : r == -2 ? "no account" : "faulted at game+" + std::to_string(g_fault_at)));
    return r;
}

void* detour_commit(ARGS12) {
    {
        static std::atomic<int> cm_logged{0};
        bool any = false;
        const int nn = g_count.load();
        for (int q = 0; q < nn; ++q) any |= g_up[q]->pend.load();
        if (cm_logged.fetch_add(1) < 100) plog(std::string("upgrades: shop commit (a stage was waiting: ") + (any ? "yes" : "no") + ", flag " + std::to_string(static_cast<int>(b & 0xFF)) + ")");
    }
    std::wstring short_msg;   // an upgrade of ours that the credits didn't cover: said in a message box after the commit
    void* short_page = nullptr;
    const int n = g_count.load();
    for (int q = 0; q < n; ++q) {
        Upgrade* u = g_up[q];
        if (!u->pend.exchange(false) || !game() || !a || (b & 0xFF)) continue;
        const long pid = u->pend_id.load();
        int st         = 0;
        if (!by_part(pid, &st) || by_part(pid, nullptr) != u) continue;
        void* pg  = *reinterpret_cast<void**>(a);
        int gid   = 0;
        void* sess = nullptr;
        if (pg) {
            sess = *reinterpret_cast<void**>(static_cast<char*>(pg) + 0xac8);
            gid  = reinterpret_cast<int (*)(void*)>(game() + 0x37AA10)(pg);
        }
        if (!pg || gid <= 0) continue;
        if (is_owned(u, gid, st)) {
            set_stage(u, gid, g_cur_ord.load(), st);
            plog("upgrades: " + u->id + " stage " + std::to_string(st) + " fitted (owned, no charge) on garage " + std::to_string(gid));
        } else if (sess && st > 0) {
            const int r = buy_guarded(sess, gid, u, st);
            if (r == 1) {
                set_stage(u, gid, g_cur_ord.load(), st);
                activate(static_cast<unsigned>(gid), "purchase");
            } else if (r == -1) {
                short_page = pg;
                short_msg  = L"You don't have enough Credits to install " + u->stages[st].name + L" (" + std::to_wstring(u->stages[st].price) +
                             L" CR, you have " + std::to_wstring(g_last_credits) + L" CR). It was not installed and you were not charged.";
            }
        }
    }
    clear_previews();
    void* res = g_orig_commit(PASS12);
    if (short_page && !short_msg.empty()) {
        if (!show_message_popup(short_page, L"INSUFFICIENT CR!", short_msg)) plog("upgrades: the not-enough-credits message box couldn't be shown");
    }
    return res;
}

// The shop's affordability check (0x16CA9F0: this, edx = total cost, r8d = Credits; cost above Credits opens the game's
// "You do not have enough Credits" message, otherwise it goes on to the "install?" prompt with that cost). The game
// sums the basket itself and knows nothing of our stages, so their price is added here: one check, one message.
void* detour_afford(ARGS12) {
    long extra = 0;
    const int n = g_count.load();
    const long gid = g_active.load();
    for (int q = 0; q < n; ++q) {
        Upgrade* u = g_up[q];
        if (!u->pend.load()) continue;
        int st = 0;
        const long pid = u->pend_id.load();
        if (!by_part(pid, &st) || by_part(pid, nullptr) != u || st <= 0 || is_owned(u, gid, st)) continue;
        extra += u->stages[st].price;
    }
    if (extra > 0) {
        static int logged = 0;
        if (logged++ < 6) plog("upgrades: the shop's cost check: " + std::to_string(static_cast<uint32_t>(b)) + " + " + std::to_string(extra) + " for the upgrade(s) of mods");
        b = static_cast<uint32_t>(static_cast<uint32_t>(b) + static_cast<uint32_t>(extra));
    }
    return g_orig_afford(PASS12);
}

void* detour_abandon(ARGS12) {
    const int n = g_count.load();
    {
        static std::atomic<int> ab_logged{0};
        bool any = false;
        for (int q = 0; q < n; ++q) any |= g_up[q]->pend.load();
        if (ab_logged.fetch_add(1) < 100) plog(std::string("upgrades: shop setup abandoned (a stage was waiting: ") + (any ? "yes" : "no") + ")");
    }
    for (int q = 0; q < n; ++q) g_up[q]->pend.store(false);
    clear_previews();
    return g_orig_abandon(PASS12);
}

// The game builds its per-part queries (stat preview, selection) with an if-chain over the enum 0..42; an enum above 42
// builds nothing, so a part of it can't be selected. Both builders get the table's last entry instead.
void* detour_qa(void* t, void* o, int e) {
    if (e > 42) e = 42;
    return g_orig_qa(t, o, e);
}
void* detour_qb(uint64_t a, uint64_t e, uint64_t c, uint64_t d, uint64_t f, uint64_t g, uint64_t h, uint64_t i, uint64_t j) {
    if (static_cast<int>(e) > 42) e = 42;
    return g_orig_qb(a, e, c, d, f, g, h, i, j);
}

void install_hooks() {
    struct H { uint32_t rva; void* detour; const char* what; void** orig; };
    const H hooks[] = {
        {0x440C0, reinterpret_cast<void*>(detour_prepare), "upgrades: SQL prepare", reinterpret_cast<void**>(&g_orig_prepare)},
        {0x16FAEF0, reinterpret_cast<void*>(detour_add), "upgrades: part add step", reinterpret_cast<void**>(&g_orig_add)},
        {0x19CDBB0, reinterpret_cast<void*>(detour_diff), "upgrades: parts differ?", reinterpret_cast<void**>(&g_orig_diff)},
        {0x4081A0, reinterpret_cast<void*>(detour_commit), "upgrades: shop commit", reinterpret_cast<void**>(&g_orig_commit)},
        {0x16CA9F0, reinterpret_cast<void*>(detour_afford), "upgrades: shop cost check", reinterpret_cast<void**>(&g_orig_afford)},
        {0x16C8C70, reinterpret_cast<void*>(detour_abandon), "upgrades: abandon setup", reinterpret_cast<void**>(&g_orig_abandon)},
        {0x16DC050, reinterpret_cast<void*>(detour_select), "upgrades: A handler", reinterpret_cast<void**>(&g_orig_select)},
        {0x1802D82, reinterpret_cast<void*>(detour_memcmp), "upgrades: memcmp thunk", reinterpret_cast<void**>(&g_orig_memcmp)},
        {0x37E980, reinterpret_cast<void*>(detour_qa), "upgrades: query builder A", reinterpret_cast<void**>(&g_orig_qa)},
        {0x410750, reinterpret_cast<void*>(detour_qb), "upgrades: query builder B", reinterpret_cast<void**>(&g_orig_qb)},
        {0x36B7A0, reinterpret_cast<void*>(detour_partset), "upgrades: part setter", reinterpret_cast<void**>(&g_orig_partset)},
        {0x38D0B0, reinterpret_cast<void*>(detour_show), "upgrades: show part on car", reinterpret_cast<void**>(&g_orig_show)},
        {0x3939F0, reinterpret_cast<void*>(detour_rebuild), "upgrades: car rebuild (log)", reinterpret_cast<void**>(&g_orig_rebuild)},
        {0x38BCD0, reinterpret_cast<void*>(detour_setup), "upgrades: setup on the shop car", reinterpret_cast<void**>(&g_orig_setup)},
        {0x16F7ED0, reinterpret_cast<void*>(detour_camera), "upgrades: shop camera for a part", reinterpret_cast<void**>(&g_orig_camera)},
    };
    for (const H& h : hooks) hook_game_function(h.rva, h.detour, h.what, h.orig);
    hook_game_function_shared(0x189BCC0, reinterpret_cast<void*>(detour_carbuild), "upgrades: a car is built (in-Car spec)", reinterpret_cast<void**>(&g_orig_carbuild));
    hook_game_function_shared(0xB26690, reinterpret_cast<void*>(detour_hud), "upgrades: the player's car (HUD controller)", reinterpret_cast<void**>(&g_orig_hud));
    install_basket_patch(reinterpret_cast<uintptr_t>(find_game()));
    // seed the car being driven from the last session, until the game's statements say which it is
    const unsigned last = GetPrivateProfileIntW(L"general", L"LastGarage", 0, ini_path().c_str());
    if (last) activate(last, "last session");
    plog("upgrades: " + std::to_string(g_count.load()) + " custom upgrade(s) hooked into the shop");
}

int slot_for(const std::string& id, int preferred) {
    const std::wstring ini = ini_path();
    wchar_t buf[16] = L"";
    GetPrivateProfileStringW(L"slots", wide(id).c_str(), L"", buf, 16, ini.c_str());
    bool used[kMaxUpgrades] = {};
    for (int s = 0; s < kMaxUpgrades; ++s) {
        wchar_t names[4096] = L"";
        // which slots the ini hands out already (by any id)
        GetPrivateProfileStringW(L"slots", nullptr, L"", names, 4096, ini.c_str());
        for (const wchar_t* p = names; *p; p += wcslen(p) + 1) {
            wchar_t v[16] = L"";
            GetPrivateProfileStringW(L"slots", p, L"", v, 16, ini.c_str());
            const int sv = _wtoi(v);
            if (v[0] && sv >= 0 && sv < kMaxUpgrades) used[sv] = true;
        }
        break;
    }
    if (buf[0]) {
        const int s = _wtoi(buf);
        if (s >= 0 && s < kMaxUpgrades) return s;
    }
    int s = -1;
    if (preferred >= 0 && preferred < kMaxUpgrades && !used[preferred]) s = preferred;
    for (int i = 0; s < 0 && i < kMaxUpgrades; ++i)
        if (!used[i]) s = i;
    if (s >= 0) WritePrivateProfileStringW(L"slots", wide(id).c_str(), wnum(s).c_str(), ini.c_str());
    return s;
}

}  // namespace

int upgrades_register(FH3Mod* mod, const FH3UpgradeDef* d, const std::function<void(const char*, uint32_t, const wchar_t*)>& add_key,
                      const std::function<void(const std::string&)>& add_sql) {
    if (!mod || !d || d->size < FH3_UPGRADEDEF_SIZE_V11 || !d->id || !*d->id || !d->name || !d->stages || d->stage_count < 2 || d->stage_count > kMaxStages) {
        plog("register_upgrade: bad definition");
        return -1;
    }
    for (const char* p = d->id; *p; ++p)
        if (!isalnum(static_cast<unsigned char>(*p)) && *p != '.' && *p != '_') {
            log_line(mod->name, "register_upgrade: the id may only hold letters, digits, '.' and '_'");
            return -1;
        }
    std::scoped_lock lk{g_reg_mu};
    const int n = g_count.load();
    for (int i = 0; i < n; ++i)
        if (g_up[i]->id == d->id) {
            log_line(mod->name, "register_upgrade: \"" + std::string(d->id) + "\" is registered already");
            return g_up[i]->slot;
        }
    if (n >= kMaxUpgrades) {
        log_line(mod->name, "register_upgrade: no free slot (8 upgrades at most)");
        return -1;
    }
    const int slot = slot_for(d->id, d->preferred_slot);
    if (slot < 0) {
        log_line(mod->name, "register_upgrade: no free slot (8 upgrades at most)");
        return -1;
    }
    for (int i = 0; i < n; ++i)
        if (g_up[i]->slot == slot) {
            log_line(mod->name, "register_upgrade: slot " + std::to_string(slot) + " is taken");
            return -1;
        }
    auto* u  = new Upgrade();
    u->mod   = mod;
    u->id    = d->id;
    u->slot  = slot;
    u->name  = d->name;
    u->desc  = d->description ? d->description : L"";
    u->area  = d->area > 0 ? d->area : 1;
    u->icon  = d->icon ? d->icon : "";
    u->camera_part = d->size >= sizeof(FH3UpgradeDef) && d->camera_part > 0 && d->camera_part <= 43 ? d->camera_part : 0;
    u->icon_lg = d->icon_large ? d->icon_large : "";
    u->cb    = d->on_stage;
    u->user  = d->user;
    for (int i = 0; i < d->stage_count; ++i) {
        Stage s;
        s.name    = d->stages[i].name ? d->stages[i].name : L"";
        s.desc    = d->stages[i].description ? d->stages[i].description : L"";
        s.price   = i ? d->stages[i].price : 0;
        s.icon    = d->stages[i].icon ? d->stages[i].icon : "";
        s.icon_lg = d->stages[i].icon_large ? d->stages[i].icon_large : "";
        u->stages.push_back(std::move(s));
    }
    const uint32_t kb = key_base(u);
    add_key("UpgradeTypes.str", kb + 0x10, u->name.c_str());
    add_key("UpgradeTypes.str", kb + 0x11, u->desc.c_str());
    for (int i = 0; i < u->count(); ++i) {
        add_key("Upgrades.str", kb + 0x20 + i, u->stages[i].name.c_str());
        add_key("Upgrades.str", kb + 0x30 + i, u->stages[i].desc.c_str());
    }
    add_sql(build_sql(u));
    g_up[n] = u;
    g_count.store(n + 1);
    log_line(mod->name, "upgrade \"" + u->id + "\" registered: slot " + std::to_string(slot) + ", part enum " + std::to_string(u->penum()) + ", " +
                            std::to_string(u->count()) + " stages");
    if (!g_started.exchange(true)) std::thread(install_hooks).detach();
    return slot;
}

static Upgrade* handle_upgrade(int h) {
    const int n = g_count.load();
    for (int i = 0; i < n; ++i)
        if (g_up[i]->slot == h) return g_up[i];
    return nullptr;
}

int upgrades_stage(int handle, int32_t garage) {
    Upgrade* u = handle_upgrade(handle);
    if (!u) return 0;
    if (garage <= 0 && g_foreign.load()) return 0;   // the driven car isn't the player's
    const long gid = garage > 0 ? garage : static_cast<long>(g_active.load());
    // In the shop, the stage being looked at, so a mod can show it on the car before it is bought.
    // Only for the rebuild the show step starts (the game builds the car within ~50 ms of it). When the cursor leaves the
    // list, the game rebuilds the car from a part set that never changed for our parts, with no call we can see, so
    // anything later than that gets the fitted stage again.
    if (const int pv = u->preview.load(); pv >= 0 && pv < u->count() && u->preview_gid.load() == gid && GetTickCount64() <= u->preview_until.load())
        return pv;
    // A stage in the basket (chosen, not installed yet) stays on the shop's car like the game's own basket parts, until
    // the setup is installed (commit) or abandoned, or another car is driven.
    if (u->pend.load() && gid == static_cast<long>(g_active.load())) {
        int st = 0;
        if (by_part(u->pend_id.load(), &st) == u) return st;
    }
    return get_stage(u, gid, 0);
}
uint32_t upgrades_active_garage() { return g_foreign.load() ? 0u : g_active.load(); }

int32_t upgrades_car_model(int32_t garage) {
    if (garage <= 0) return 0;
    std::scoped_lock lk{g_car_mu};
    const int n = g_count.load();
    for (int i = 0; i < n; ++i) {
        const long o = car_locked(g_up[i], garage).ord;
        if (o > 0) return static_cast<int32_t>(o);
    }
    std::scoped_lock lk2{g_gar_mu};
    const auto it = g_gar_car.find(garage);
    return it != g_gar_car.end() ? static_cast<int32_t>(car_model(it->second)) : 0;
}

// ---- is the car being built the player's? ---------------------------------------------------------------------
// The game builds every car through game+0x189BCC0 (rcx = the new Car; the spec the loader fills is Car+0x140), the
// player's and the AI's alike, and the spec can't be changed later with any effect. An AI car of the player's model
// (a one-make race) would get the player's custom upgrades. The platform keeps the player's Car (the one it accepted
// last, or the one the HUD shows) and decides per build: a new car of that model while the player's car of that
// model is still on the road (same garage car, object alive) is someone else's; the upgrade shop's car, a car
// after a switch to another garage car, or the first car of a model is the player's.
thread_local char* t_build_car    = nullptr;   // the Car being built on this thread
thread_local int t_build_verdict  = -1;        // -1 not decided yet, 0 not the player's, 1 the player's
std::mutex g_pc_mu;
char* g_pc_car  = nullptr;   // the player's Car
long g_pc_gid   = 0;         // its garage id
Fn12 g_orig_carbuild = nullptr;

bool car_alive(const char* car, long model) {
    char* g = game();
    if (!g || !car || !readable(car, 0x118)) return false;
    if (*reinterpret_cast<const uintptr_t*>(car) != reinterpret_cast<uintptr_t>(g) + 0x42E0650) return false;   // Car's vtable (a destroyed one has a base's)
    const char* spec = *reinterpret_cast<char* const*>(car + 0x110);
    if (!spec || !readable(spec + 0x30, 4)) return true;   // not complete yet: still counts
    return car_model(*reinterpret_cast<const int32_t*>(spec + 0x30)) == model;
}

bool build_is_players(long gid, long model) {
    if (t_build_verdict >= 0) return t_build_verdict == 1;
    char* const c      = t_build_car;
    const uint64_t now = GetTickCount64();
    const char* why    = nullptr;
    bool mine          = true;
    {
        std::scoped_lock lk{g_pc_mu};
        const uint64_t ex = g_expect_build.load();
        if (now - g_shop_tick.load() < 5000) why = "the upgrade shop's car";
        else if (ex && now - ex < 180000 && static_cast<unsigned>(gid) == g_active.load()) { why = "a new car for the player"; g_expect_build.store(0); }
        else if (g_pc_car && g_pc_car != c && g_pc_gid == gid && car_alive(g_pc_car, model)) { mine = false; why = "the player's car of that model is on the road already"; }
        else why = "no car of the player's of that model on the road";
        if (mine) { g_pc_car = c; g_pc_gid = gid; }
    }
    t_build_verdict = mine ? 1 : 0;
    static std::atomic<int> lines{0};
    if (lines.fetch_add(1) < 200) {
        char b[220];
        snprintf(b, sizeof b, "upgrades: car %p (model %ld) is built: %s (%s)", static_cast<void*>(c), model, mine ? "the player's, garage car " : "someone else's, not garage car ", why);
        plog(std::string(b) + (mine ? std::to_string(gid) : std::to_string(gid)));
    }
    return mine;
}

void* detour_carbuild(ARGS12) {
    char* const prev_car = t_build_car;
    const int prev_v     = t_build_verdict;
    t_build_car          = reinterpret_cast<char*>(a);
    t_build_verdict      = -1;
    void* r              = g_orig_carbuild(PASS12);
    t_build_car          = prev_car;
    t_build_verdict      = prev_v;
    return r;
}

int32_t upgrades_garage_for_car(int32_t car_id, uint32_t flags) {
    if (car_id <= 0) return 0;
    const long want   = car_model(car_id);
    const unsigned act = g_active.load();
    bool known = false, act_in = false;
    long best = 0, cands = 0;
    uint64_t best_t = 0;
    {
        std::scoped_lock lk{g_gar_mu};
        known = !g_gar_car.empty();
        for (const auto& [g, c] : g_gar_car) {
            if (car_model(c) != want) continue;
            ++cands;
            if (static_cast<unsigned>(g) == act) act_in = true;
            const auto t = g_gar_touch.find(g);
            const uint64_t tt = t != g_gar_touch.end() ? t->second : 0;
            if (!best || tt > best_t) { best = g; best_t = tt; }
        }
    }
    if (!known) {   // the garage isn't read yet: the model recorded for the driven car decides, as before API 14
        const int32_t m = act ? upgrades_car_model(static_cast<int32_t>(act)) : 0;
        return act && (m <= 0 || car_model(m) == want) ? static_cast<int32_t>(act) : 0;
    }
    if (act_in) return t_build_car && !build_is_players(static_cast<long>(act), want) ? 0 : static_cast<int32_t>(act);
    if (!cands) return 0;   // none of the player's cars is that model: a loaner, an AI car
    const char* why = nullptr;
    if (best_t && GetTickCount64() - best_t < 60000) why = "the game read it just before";
    else if (flags & FH3MOD_CAR_IS_PLAYERS) why = "the player's car is that model";
    if (!why) return 0;
    if (t_build_car && !build_is_players(best, want)) return 0;
    if (flags & FH3MOD_CAR_MAKE_DRIVEN) {
        const std::string r = std::string("model ") + std::to_string(want) + ", " + why;
        activate(static_cast<unsigned>(best), r.c_str());
    }
    return static_cast<int32_t>(best);
}

int upgrades_stage_for_car(int handle, int32_t car_id) {
    const int32_t g = upgrades_garage_for_car(car_id, FH3MOD_CAR_MAKE_DRIVEN);
    return g > 0 ? upgrades_stage(handle, g) : 0;
}

// ---- the car the player drives ----------------------------------------------------------------------------------
// The HUD speedometer controller (game+0xB26690, rcx = controller) holds a weak pointer to the player's Car at
// +0x120 (+0x128 its control block); the Car's spec (+0x110) holds the Data_Car id at +0x30. A few times a second
// the platform checks which car that is: one of the player's (it becomes the driven car, so every mod's stage
// callback and upgrade_stage follow a car switch at once) or none of them (a loaner: no custom upgrades on it).

void driven_car_seen(int32_t car_id, char* car) {
    static std::atomic<int32_t> last{0};
    if (car_id < 100 || car_id >= 100000000) return;
    bool known = false;
    {
        std::scoped_lock lk{g_gar_mu};
        known = !g_gar_car.empty();
    }
    if (!known) return;   // the garage isn't read yet: nothing to compare with
    const int32_t g = upgrades_garage_for_car(car_id, FH3MOD_CAR_MAKE_DRIVEN | FH3MOD_CAR_IS_PLAYERS);
    if (g > 0 && car) {
        std::scoped_lock lk{g_pc_mu};
        g_pc_car = car;
        g_pc_gid = g;
    }
    if (g > 0) {
        if (g_foreign.load()) activate(static_cast<unsigned>(g), "the player's car again");
    } else if (!g_foreign.exchange(true)) {
        plog("upgrades: the driven car (Data_Car " + std::to_string(car_id) + ") isn't one of the player's cars: no custom upgrades on it");
        clear_previews();
        const int n = g_count.load();
        for (int i = 0; i < n; ++i) notify(g_up[i], 0, 0);
    }
    if (last.exchange(car_id) != car_id)
        plog("upgrades: the player drives Data_Car " + std::to_string(car_id) + (g > 0 ? ", garage car " + std::to_string(g) : std::string(", not one of the player's cars")));
}

uint64_t detour_hud(void* c, void* a, void* b, void* d) {
    const uint64_t r = g_orig_hud(c, a, b, d);
    static std::atomic<uint64_t> next{0};
    const uint64_t now = GetTickCount64();
    if (now < next.load()) return r;
    next.store(now + 250);
    if (!c || !readable(static_cast<char*>(c) + 0x120, 16)) return r;
    char* car = *reinterpret_cast<char**>(static_cast<char*>(c) + 0x120);
    void* cb  = *reinterpret_cast<void**>(static_cast<char*>(c) + 0x128);
    if (!car || !cb || !readable(car + 0x110, 8)) return r;
    char* spec = *reinterpret_cast<char**>(car + 0x110);
    if (!spec || !readable(spec + 0x30, 4)) return r;
    driven_car_seen(*reinterpret_cast<int32_t*>(spec + 0x30), car);
    return r;
}

void upgrades_import(int handle, int32_t garage, int32_t ordinal, int stage, uint32_t owned_mask_in) {
    Upgrade* u = handle_upgrade(handle);
    if (!u || garage <= 0 || stage < 0 || stage >= u->count()) return;
    std::scoped_lock lk{g_car_mu};
    Car& c  = car_locked(u, garage);
    c.stage = stage;
    c.owned = (owned_mask_in | (1u << stage) | 1u) & ((1u << u->count()) - 1);
    c.ord   = ordinal > 0 ? ordinal : c.ord;
    save_car(u, garage, c);
}

// The sell-car module needs the SQL hook even when no upgrade is registered.
void upgrades_start_hooks() {
    if (!g_started.exchange(true)) std::thread(install_hooks).detach();
}
void* upgrades_live_db() { return g_live_db.load(); }


}  // namespace fh3platform
