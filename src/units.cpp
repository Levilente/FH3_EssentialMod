// Speed and power units chosen on their own (Settings > HUD and Gameplay, under Units).
//
// The game keeps one table of display units: 35 rows ("kinds") for each unit system, 20 bytes a row, at the
// pointer in .data (RVA 0x4E7DDB0). A row's unit id (+0xC), its number format (+0x10) and its label (+0x18)
// decide what is shown and how the value is converted. The Units option only picks the system (Imperial or
// Metric), so Metric gives km/h but also kW. Here the rows for speed and for power are copied from rows of
// the system that has the unit asked for, over every system, so the two are chosen independently.
// "Automatic" puts the game's own rows back.
#include "platform.hpp"

#include <windows.h>
#include <cstdio>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fh3platform {

namespace {

constexpr std::uintptr_t RVA_UNIT_TABLE = 0x4E7DDB0;
constexpr int kSystems = 4;
constexpr int kKinds   = 35;
constexpr int kRowSize = 20;

enum : uint32_t { U_MPH = 22, U_KMH = 23, U_HP = 25, U_W = 26, U_KW = 27, U_PS = 28 };

struct Snap {
    int sys, kind;
    uint32_t id, fmt, label;
    bool power;
};

std::mutex g_mu;
std::vector<Snap> g_snap;            // the game's own rows for speed and power
std::atomic<int> g_choice[2] = {0, 0};  // 0 speed, 1 power; 0 = automatic
std::atomic<bool> g_started{false};

std::wstring ini_path() { return (azimuth_dir() / L"fh3-mods" / L"platform-units.ini").wstring(); }

bool readable(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return static_cast<const uint8_t*>(p) + n <= static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

uint8_t* table() {
    auto* base = static_cast<uint8_t*>(find_game());
    if (!base || !readable(base + RVA_UNIT_TABLE, 8)) return nullptr;
    return *reinterpret_cast<uint8_t**>(base + RVA_UNIT_TABLE);
}

uint8_t* row(uint8_t* t, int sys, int kind) { return t + kRowSize * (sys * kKinds + kind) + 8; }

bool is_speed(uint32_t id) { return id == U_MPH || id == U_KMH; }
bool is_power(uint32_t id) { return id == U_HP || id == U_W || id == U_KW || id == U_PS; }

// finds the game's rows, once the table is loaded
bool take_snapshot(uint8_t* t) {
    std::vector<Snap> s;
    for (int sys = 0; sys < kSystems; ++sys) {
        uint8_t* first = row(t, sys, 0);
        uint8_t* last  = row(t, sys, kKinds - 1);
        if (!readable(first, kRowSize) || !readable(last, kRowSize)) break;
        for (int k = 0; k < kKinds; ++k) {
            const uint8_t* r = row(t, sys, k);
            uint32_t id, fmt, label;
            std::memcpy(&id, r + 4, 4);
            std::memcpy(&fmt, r + 8, 4);
            std::memcpy(&label, r + 0x10, 4);
            if (is_speed(id)) s.push_back({sys, k, id, fmt, label, false});
            else if (is_power(id)) s.push_back({sys, k, id, fmt, label, true});
        }
    }
    {
        char b[160];
        std::snprintf(b, sizeof b, "unit table at %p: %zu speed/power rows", static_cast<void*>(t), s.size());
        log_line("units", b);
        for (int sys = 0; sys < kSystems && readable(row(t, sys, 0), kRowSize); ++sys) {
            std::string ids;
            for (int k = 0; k < kKinds; ++k) {
                uint32_t id;
                std::memcpy(&id, row(t, sys, k) + 4, 4);
                ids += std::to_string(id) + " ";
            }
            log_line("units", "system " + std::to_string(sys) + " ids: " + ids);
        }
        for (const Snap& r : s)
            log_line("units", "row sys " + std::to_string(r.sys) + " kind " + std::to_string(r.kind) + " id " + std::to_string(r.id));
    }
    if (s.empty()) return false;
    std::scoped_lock lk{g_mu};
    g_snap = std::move(s);
    return true;
}

uint32_t wanted_id(bool power, int choice) {
    if (choice == 0) return 0;
    if (power) return choice == 1 ? U_HP : U_KW;
    return choice == 1 ? U_KMH : U_MPH;
}

void write_rows(uint8_t* t) {
    std::vector<Snap> snap;
    {
        std::scoped_lock lk{g_mu};
        snap = g_snap;
    }
    for (int pw = 0; pw < 2; ++pw) {
        const uint32_t want = wanted_id(pw != 0, g_choice[pw]);
        const Snap* donor   = nullptr;
        if (want)
            for (const Snap& s : snap)
                if (s.power == (pw != 0) && s.id == want) { donor = &s; break; }
        for (const Snap& s : snap) {
            if (s.power != (pw != 0)) continue;
            // only the unit of the kind, not the other (m/s, W) rows of the table
            if (!pw && !is_speed(s.id)) continue;
            const Snap& use = donor ? *donor : s;
            uint8_t* r      = row(t, s.sys, s.kind);
            __try {
                if (*reinterpret_cast<uint32_t*>(r + 4) != use.id)
                    log_line("units", "sys " + std::to_string(s.sys) + " kind " + std::to_string(s.kind) + ": " + std::to_string(s.id) + " -> " + std::to_string(use.id));
                if (*reinterpret_cast<uint32_t*>(r + 4) != use.id) *reinterpret_cast<uint32_t*>(r + 4) = use.id;
                if (*reinterpret_cast<uint32_t*>(r + 8) != use.fmt) *reinterpret_cast<uint32_t*>(r + 8) = use.fmt;
                if (*reinterpret_cast<uint32_t*>(r + 0x10) != use.label) *reinterpret_cast<uint32_t*>(r + 0x10) = use.label;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
}

void worker() {
    bool have = false;
    int idle  = 0;
    for (;;) {
        uint8_t* t = table();
        if (idle < 6 || (idle % 30) == 0) {
            char b[120];
            std::snprintf(b, sizeof b, "table pointer %p, readable %d, snapshot %d", static_cast<void*>(t), t && readable(t, 64) ? 1 : 0, have ? 1 : 0);
            log_line("units", b);
            auto* base = static_cast<uint8_t*>(find_game());
            MEMORY_BASIC_INFORMATION mbi{};
            if (!base) { log_line("units", "the game isn't found yet"); }
            const bool ok = base && VirtualQuery(base + 0x4E7DD90, &mbi, sizeof mbi) != 0;
            std::string q = "module " + std::to_string(reinterpret_cast<std::uintptr_t>(base)) + ", query " + (ok ? "ok" : "failed") +
                            ", state " + std::to_string(ok ? mbi.State : 0) + ", protect " + std::to_string(ok ? mbi.Protect : 0) +
                            ", region " + std::to_string(ok ? reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) : 0) + "+" +
                            std::to_string(ok ? mbi.RegionSize : 0) + ", qwords:";
            if (base && readable(base + 0x4E7DD90, 64))
                for (int i = 0; i < 8; ++i) {
                    char h[32];
                    std::snprintf(h, sizeof h, " %llx", *reinterpret_cast<unsigned long long*>(base + 0x4E7DD90 + 8 * i));
                    q += h;
                }
            log_line("units", q);
        }
        ++idle;
        if (t && readable(t, 64)) {
            if (!have) have = take_snapshot(t);
            // the table is checked now and then: if the game loads it again, the choice goes back in
            if (have) write_rows(t);
        }
        Sleep(have ? 2000 : 500);
        (void)idle;
    }
}

} // namespace

int units_get(int which) { return which == 0 || which == 1 ? g_choice[which].load() : 0; }

void units_set(int which, int v) {
    if ((which != 0 && which != 1) || v < 0 || v > 2) return;
    g_choice[which] = v;
    WritePrivateProfileStringW(L"units", which ? L"Power" : L"Speed", std::to_wstring(v).c_str(), ini_path().c_str());
    if (uint8_t* t = table(); t && readable(t, 64)) write_rows(t);
}

void units_start() {
    const std::wstring ini = ini_path();
    g_choice[0] = static_cast<int>(GetPrivateProfileIntW(L"units", L"Speed", 0, ini.c_str()));
    g_choice[1] = static_cast<int>(GetPrivateProfileIntW(L"units", L"Power", 0, ini.c_str()));
    for (auto& c : g_choice) if (c < 0 || c > 2) c = 0;
    if (g_started.exchange(true)) return;
    std::thread(worker).detach();
}

} // namespace fh3platform
