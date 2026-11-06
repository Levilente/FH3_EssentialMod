// Free revving in the garage's upgrade shop: hold the right trigger anywhere in Upgrades and the engine revs.
// STATUS: experiment, off by default (platform-rev.ini [rev] Enabled=1). The engine torque step (RVA_TORQUE) is not
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
std::atomic<uint64_t> g_pad_tick{0};
std::atomic<float> g_rt{0.0f};

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

bool detour_torque(void* self, float rpm, float throttle, void* r8, void* r9, uint64_t a5, uint64_t a6, uint64_t a7, uint64_t a8) {
    g_calls.fetch_add(1);
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
    if (!g_enabled.load()) return;
    std::thread([] {   // diagnostics: while RT is held, say what the hook sees
        uint64_t last = 0;
        int n = 0;
        for (;;) {
            Sleep(1000);
            if (n >= 40) return;
            const float rt = trigger();
            const uint64_t c = g_calls.load();
            if (rt < 0.3f) { last = c; continue; }
            const uint64_t t = upgrades_shop_tick(), now = GetTickCount64();
            char b[200];
            snprintf(b, sizeof b, "rev: RT %.2f held; torque calls in the last second %llu; shop touched the car %lld ms ago",
                     static_cast<double>(rt), static_cast<unsigned long long>(c - last), t ? static_cast<long long>(now - t) : -1LL);
            plog(b);
            last = c;
            ++n;
        }
    }).detach();
    hook_game_function(RVA_TORQUE, reinterpret_cast<void*>(detour_torque), "rev: engine torque", reinterpret_cast<void**>(&g_orig));
}

} // namespace fh3platform
