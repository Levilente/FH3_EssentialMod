// Free revving in the garage's upgrade shop: hold the right trigger anywhere in Upgrades and the engine revs.
// STATUS: research, off by default (platform-rev.ini [rev] Enabled=1 / Probe=1). Findings in docs/fh3-game-notes.md.
// run for the garage car at all, so a raised throttle changes nothing there; see docs/fh3-game-notes.md.
//
// The shop's car is simulated like any other, but nothing drives it, so its throttle is 0. The engine's torque
// (RVA_TORQUE: this = car dynamics, xmm1 = revs, xmm2 = throttle, r9 = output) takes the throttle as an argument;
// while the shop is in use (it touched the car a moment ago) the throttle is raised to the trigger's value.
#include "platform.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

namespace fh3platform {

namespace {

constexpr uint32_t RVA_TORQUE = 0x1D278C0;
constexpr uint64_t kShopMs    = 8000;   // the shop counts as in use this long after it last touched the car

struct PadState {
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, PadState*);
XInputGetStateFn g_xinput = nullptr;

using TorqueFn = bool (*)(void*, float, float, void*, void*, uint64_t, uint64_t, uint64_t, uint64_t);
TorqueFn g_orig = nullptr;
std::atomic<int> g_enabled{0};
std::atomic<int> g_logged{0};
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_last_torque_ms{0};   // the engine step last ran for some car (driving, traffic ...)
std::atomic<uint64_t> g_pad_tick{0};
std::atomic<float> g_rt{0.0f};

bool readable(const void* p, std::size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

float trigger() {
    const uint64_t now = GetTickCount64();
    if (now - g_pad_tick.load() >= 8) {
        g_pad_tick = now;
        if (!g_xinput) {
            HMODULE x = LoadLibraryW(L"xinput1_4.dll");
            if (!x) x = LoadLibraryW(L"xinput9_1_0.dll");
            if (x) g_xinput = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(x, "XInputGetState")));
        }
        PadState st{};
        float v = 0.0f;
        if (g_xinput && g_xinput(0, &st) == ERROR_SUCCESS) v = st.rt / 255.0f;
        g_rt = v < 0.06f ? 0.0f : v;
    }
    return g_rt.load();
}


// ---- Probe: why does the garage car run no engine step? Snapshots of a driven car and of the garage car, compared ----
// The driven car is the one the engine torque hook sees with the trigger held (shop idle for 20 s); the garage car is any
// car whose tick (Release, slot 3 of the car's second vtable) runs while the shop is in use. Dwords that are small in both
// (flags, states, counts) and differ are logged once.
constexpr uintptr_t kCarVtable2 = 0x4682608;
constexpr int kCarSecondary = 0x4B0, kSnap = 0x5000;
unsigned char g_drv[kSnap], g_gar[3][kSnap];
void* g_gar_car[3] = {nullptr, nullptr, nullptr};
std::atomic<bool> g_have_drv{false}, g_diffed{false};
std::atomic<int> g_n_gar{0};
std::atomic<int> g_n_snap{0};   // snapshots taken (never reset)
std::atomic<uint64_t> g_last_shop_logged{0};
std::atomic<void*> g_drv_car{nullptr};
std::atomic<int> g_probe{0};
using RelFn = uint64_t (*)(void*);
RelFn g_orig_rel = nullptr;

void log_diff() {
    if (g_diffed.exchange(true)) return;
    for (int k = 0; k < g_n_snap.load(); ++k) {
        std::string out = "rev probe: garage car " + std::to_string(k) + " vs driven car, small dwords that differ (offset garage->driven):";
        int n = 0;
        for (int o = 0; o + 4 <= kSnap && n < 160; o += 4) {
            uint32_t a, b;
            memcpy(&a, g_gar[k] + o, 4);
            memcpy(&b, g_drv + o, 4);
            if (a == b || a > 16 || b > 16) continue;
            char t[40];
            snprintf(t, sizeof t, " +0x%X(%u->%u)", o, a, b);
            out += t;
            ++n;
        }
        plog(out);
    }
}


// Experiments on the garage cars: each press of the trigger in the shop sets one group of the flags found above (and puts
// them back when the trigger is released); the log says what the engine step did meanwhile.
struct Poke { int off; uint32_t val; };
const Poke kExp0[] = {{0x4990, 0}};
const Poke kExp1[] = {{0x160, 4}};
const Poke kExp2[] = {{0x4990, 0}, {0x160, 4}};
const Poke kExp3[] = {{0x1724, 1}, {0x19FC, 1}, {0x1D88, 1}, {0x1D8C, 2}, {0x3218, 1}, {0x321C, 2}, {0x46A8, 1}, {0x46AC, 2}};
const Poke kExp4[] = {{0x4990, 0}, {0x160, 4}, {0x1724, 1}, {0x19FC, 1}, {0x1D88, 1}, {0x1D8C, 2}, {0x3218, 1}, {0x321C, 2}, {0x46A8, 1}, {0x46AC, 2}};
struct ExpDef { const Poke* p; int n; const char* name; };
const ExpDef kExps[] = {{kExp0, 1, "+0x4990=0"}, {kExp1, 1, "+0x160=4"}, {kExp2, 2, "+0x4990=0, +0x160=4"},
                        {kExp3, 8, "the six pairs (+0x1724 ... +0x46AC)"}, {kExp4, 10, "all of them"}};
constexpr int kNExp = 1;   // the first one turned out to release the car (it drove off)
int g_exp = 0;                       // next experiment
bool g_exp_on = false;
uint32_t g_saved[3][16];
std::atomic<uint64_t> g_gcalls{0};
std::atomic<float> g_gpeak{0.0f};
uint64_t g_exp_calls0 = 0;
void* g_live[3] = {nullptr, nullptr, nullptr};   // cars whose engine step ran during the experiment (the shown car)
bool is_live(const void* c) { for (void* l : g_live) if (l == c) return true; return false; }

bool is_garage_car(const void* c) {
    for (int k = 0; k < g_n_gar.load(); ++k) if (g_gar_car[k] == c) return true;
    return false;
}

constexpr int kMaxExp = 8;
char* g_exp_cars[kMaxExp];
uint32_t g_exp_saved[kMaxExp][16];
int g_exp_n = 0;

void exp_apply(char* c) {   // put the experiment's values on one car (once), remembering what was there
    if (g_exp_n >= kMaxExp || !readable(c, kSnap)) return;
    for (int k = 0; k < g_exp_n; ++k) if (g_exp_cars[k] == c) return;
    const ExpDef& e = kExps[g_exp % kNExp];
    for (int i = 0; i < e.n; ++i) {
        memcpy(&g_exp_saved[g_exp_n][i], c + e.p[i].off, 4);
        memcpy(c + e.p[i].off, &e.p[i].val, 4);
    }
    g_exp_cars[g_exp_n++] = c;
    char b[120];
    snprintf(b, sizeof b, "rev probe: experiment value set on car %p", static_cast<void*>(c));
    plog(b);
}

void exp_step(bool in_garage) {
    const float rt = trigger();
    if (!g_exp_on && rt > 0.8f && in_garage) {
        const ExpDef& e = kExps[g_exp % kNExp];
        g_exp_n = 0;
        for (void*& l : g_live) l = nullptr;
        g_exp_on = true;
        g_gcalls = 0;
        g_gpeak = 0.0f;
        for (int k = 0; k < g_n_gar.load(); ++k) exp_apply(static_cast<char*>(g_gar_car[k]));
        char b[160];
        snprintf(b, sizeof b, "rev probe: press %d: experiment %d (%s) applied to %d car(s)", g_exp + 1, g_exp % kNExp, e.name, g_exp_n);
        plog(b);
    } else if (g_exp_on && rt < 0.2f) {
        const ExpDef& e = kExps[g_exp % kNExp];
        for (int k = 0; k < g_exp_n; ++k)
            if (readable(g_exp_cars[k], kSnap)) for (int i = 0; i < e.n; ++i) memcpy(g_exp_cars[k] + e.p[i].off, &g_exp_saved[k][i], 4);
        char b[160];
        snprintf(b, sizeof b, "rev probe: press %d done: the engine step ran %llu times for the cars, highest revs %.1f",
                 g_exp + 1, static_cast<unsigned long long>(g_gcalls.load()), static_cast<double>(g_gpeak.load()));
        plog(b);
        g_exp_on = false;
        g_exp_n = 0;
        for (void*& l : g_live) l = nullptr;
        ++g_exp;
    }
}

bool not_driving() { return GetTickCount64() - g_last_torque_ms.load() > 1500; }   // no engine step anywhere: the garage, a menu

uint64_t detour_rel(void* self) {
    char* car = static_cast<char*>(self) - kCarSecondary;
    const uint64_t shop = upgrades_shop_tick();
    if (!g_exp_on && g_n_gar.load() > 0 && !not_driving()) g_n_gar.store(0);   // the garage cars are gone: no stale pointers
    if (g_probe.load() && g_last_torque_ms.load() && not_driving() && g_n_gar.load() < 3) {
        bool known = false;
        for (int k = 0; k < g_n_gar.load(); ++k) known |= g_gar_car[k] == car;
        if (!known && readable(car, kSnap)) {
            const int k = g_n_gar.load();
            memcpy(g_gar[k], car, kSnap);
            g_gar_car[k] = car;
            g_n_gar.store(k + 1);
            if (k + 1 > g_n_snap.load()) g_n_snap.store(k + 1);
            char b[120];
            snprintf(b, sizeof b, "rev probe: garage car %d is %p (ticking while nothing is driven)", k, static_cast<void*>(car));
            plog(b);
        }
    }
    if (g_exp_on && g_exp_n < kMaxExp && not_driving()) exp_apply(car);   // the shown car may not be one we captured
    if (g_exp_on && is_live(car) && readable(car, 0x40)) {   // hold the car in place: no velocity, the wheels and the engine do their thing
        float* v = reinterpret_cast<float*>(car + 0x10);
        v[0] = v[1] = v[2] = 0.0f;
    }
    if (g_have_drv.load() && g_n_snap.load() > 0) log_diff();
    if (g_have_drv.load() && g_n_gar.load() > 0) exp_step(not_driving());
    if (shop && shop != g_last_shop_logged.load() && GetTickCount64() - shop < 100 && g_n_gar.load() == 0 && g_have_drv.load()) {
        g_last_shop_logged.store(shop);
        static int n = 0;
        if (n++ < 6) plog("rev probe: the shop touched the car, waiting for a garage car tick");
    }
    return g_orig_rel(self);
}

bool detour_torque(void* self, float rpm, float throttle, void* r8, void* r9, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8) {
    g_calls.fetch_add(1);
    if (!is_garage_car(self) && !(g_exp_on && is_live(self))) g_last_torque_ms.store(GetTickCount64());
    if (g_exp_on && GetTickCount64() - g_last_torque_ms.load() > 0 && !is_live(self)) {
        for (void*& l : g_live) if (!l) { l = self; break; }
    }
    if (g_exp_on && is_live(self)) {
        g_gcalls.fetch_add(1);
        if (rpm > g_gpeak.load()) g_gpeak = rpm;
        const float rt = trigger();
        if (rt > throttle) throttle = rt;
    }
    if (g_probe.load() && !g_have_drv.load()) {
        const uint64_t shop = upgrades_shop_tick();
        if (throttle > 0.8f && trigger() > 0.8f && readable(self, kSnap)) {
            memcpy(g_drv, self, kSnap);
            g_drv_car.store(self);
            g_have_drv.store(true);
            char b[120];
            snprintf(b, sizeof b, "rev probe: driven car is %p (trigger held, shop idle)", self);
            plog(b);
        }
    }
    if (g_enabled.load() && GetTickCount64() - upgrades_shop_tick() < kShopMs && upgrades_shop_tick()) {
        const float rt = trigger();
        if (rt > throttle) {
            if (g_logged.fetch_add(1) < 12) {
                char b[160];
                snprintf(b, sizeof b, "rev: engine %p, revs %.1f, throttle %.2f -> %.2f", self, static_cast<double>(rpm),
                         static_cast<double>(throttle), static_cast<double>(rt));
                plog(b);
            }
            throttle = rt;
        }
    }
    return g_orig(self, rpm, throttle, r8, r9, a5, a6, a7, a8);
}

} // namespace

void rev_start() {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) return;
    const std::wstring ini = (azimuth_dir() / L"fh3-mods" / L"platform-rev.ini").wstring();
    g_enabled = static_cast<int>(GetPrivateProfileIntW(L"rev", L"Enabled", 0, ini.c_str()));
    g_probe = static_cast<int>(GetPrivateProfileIntW(L"rev", L"Probe", 0, ini.c_str()));
    if (!g_enabled.load() && !g_probe.load()) return;
    std::thread([] {
        char* base = nullptr;
        for (int i = 0; i < 2400 && !base; ++i) {   // the game maps late; wait until the car vtable has its entries
            base = static_cast<char*>(find_game());
            if (base && !(readable(base + kCarVtable2, 64) && *reinterpret_cast<void**>(base + kCarVtable2 + 24))) base = nullptr;
            if (!base) Sleep(250);
        }
        if (base && g_probe.load()) {
            void** slot = reinterpret_cast<void**>(base + kCarVtable2) + 3;   // Release, the end of the car's tick
            DWORD old = 0;
            if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) {
                g_orig_rel = reinterpret_cast<RelFn>(*slot);
                *slot = reinterpret_cast<void*>(&detour_rel);
                VirtualProtect(slot, sizeof(void*), old, &old);
                plog("rev probe: watching the car tick");
            }
        }
        hook_game_function(RVA_TORQUE, reinterpret_cast<void*>(detour_torque), "rev: engine torque", reinterpret_cast<void**>(&g_orig));
    }).detach();
}

} // namespace fh3platform
