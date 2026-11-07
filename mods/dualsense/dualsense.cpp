// DualSense for Forza Horizon 3 (FH3 Mod Platform mod).
//
// Talks to a DualSense / DualSense Edge over USB (HID output reports) and feels the player's car through the game's own data:
//  - light bar: a colour that goes to red towards the redline and flashes on the rev limiter; player lights as a rev counter;
//  - adaptive triggers: L2 is a brake pedal with a progressive resistance, R2 vibrates on the rev limiter; off outside driving;
//  - rumble (the controller's motors): the engine's hum at its revs, a knock at every gear change, a buzz on the limiter, a thump
//    on impacts and landings (a sudden change of the car's velocity).
// The controller's input is not read here (the platform hands every controller to the mods; the game gets the pad the way it
// always did), so nothing about the game's controls changes. Nothing is written to the game's folder or files.
//
// How the car is read (the same addresses as the Launch Control mod): Car::Update (game+0x18B8D20, once a frame) is hooked and
// the HUD controller (game+0xB26690) says which Car is the player's. Car+0x71E8 is the dynamics interface (CCarDynamics+0x4B0):
// +0x1D4 the engine speed (rad/s), +0xAA8 the gear (low nibble), CCarDynamics+0x684 the engine's speed limit, +0x1748 the raw
// throttle, +0x10 the velocity.
//
// USB and Bluetooth. Over Bluetooth the first output report switches the controller to its full input reports (0x31); the platform
// reads those for the mods itself. Close Steam's PlayStation controller support and DS4Windows while playing.
#include "fh3mod.h"

#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr size_t kOffIface   = 0x71E8;
constexpr size_t kIfaceToDyn = 0x4B0;
constexpr size_t kDynVelocity = 0x10;
constexpr size_t kIfOmega    = 0x1D4;
constexpr size_t kIfGear     = 0xAA8;
constexpr size_t kDynEngMax  = 0x684;
constexpr size_t kDynThrRaw  = 0x1748;
constexpr uintptr_t kRvaCarUpdate = 0x18B8D20;
constexpr uintptr_t kRvaHudCtrl   = 0xB26690;
constexpr float kRadPerSecToRpm   = 9.549296586f;

const FH3ModApi* g_api = nullptr;
FH3Mod* g_mod          = nullptr;
char* g_game           = nullptr;
std::wstring g_cfg_path;

// ---- settings (fh3-mods\DualSense.ini; the same rows are on the Mods page) -----------------------------------------------------
std::atomic<bool> g_enabled{true};
std::atomic<float> g_rumble{1.0f};         // 0..2
std::atomic<bool> g_triggers{true};
std::atomic<float> g_trig_strength{0.8f};  // 0..1
std::atomic<bool> g_lights{true};
std::atomic<bool> g_rev_lights{true};
std::atomic<bool> g_bluetooth{true};
bool g_verbose = false;
int g_color[3] = {0, 90, 255};             // the light bar's colour at low revs

bool is_problem(const char* s) {
    static const char* const words[] = {"could not", "couldn't", "can't", "cannot", "failed", "not found", "error", "not supported"};
    char low[256];
    size_t i = 0;
    for (; s[i] && i < sizeof low - 1; ++i) low[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    low[i] = 0;
    for (const char* w : words)
        if (std::strstr(low, w)) return true;
    return false;
}
void say(const char* fmt, ...) {
    char line[500];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_api && g_mod && (g_verbose || is_problem(line) || std::strncmp(line, "controller", 10) == 0)) g_api->log(g_mod, line);
}

float cfg_float(const wchar_t* key, float def) {
    wchar_t buf[64] = L"";
    GetPrivateProfileStringW(L"DualSense", key, L"", buf, 64, g_cfg_path.c_str());
    if (buf[0]) return static_cast<float>(std::wcstod(buf, nullptr));
    wchar_t v[48];
    _snwprintf(v, 48, L"%g", static_cast<double>(def));
    WritePrivateProfileStringW(L"DualSense", key, v, g_cfg_path.c_str());
    return def;
}
void save_float(const wchar_t* key, float v) {
    wchar_t s[48];
    _snwprintf(s, 48, L"%g", static_cast<double>(v));
    WritePrivateProfileStringW(L"DualSense", key, s, g_cfg_path.c_str());
}
float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

void load_config() {
    g_enabled.store(cfg_float(L"Enabled", 1) != 0);
    g_rumble.store(clampf(cfg_float(L"Rumble", 1.0f), 0, 2));
    g_triggers.store(cfg_float(L"Triggers", 1) != 0);
    g_trig_strength.store(clampf(cfg_float(L"TriggerStrength", 0.8f), 0, 1));
    g_lights.store(cfg_float(L"LightBar", 1) != 0);
    g_rev_lights.store(cfg_float(L"RevLights", 1) != 0);
    g_bluetooth.store(cfg_float(L"Bluetooth", 1) != 0);
    g_color[0] = static_cast<int>(clampf(cfg_float(L"ColorR", 0), 0, 255));
    g_color[1] = static_cast<int>(clampf(cfg_float(L"ColorG", 90), 0, 255));
    g_color[2] = static_cast<int>(clampf(cfg_float(L"ColorB", 255), 0, 255));
    g_verbose = cfg_float(L"Log", 0) != 0;
}

void on_toggle(void*, const char* key, int value) {
    if (!std::strcmp(key, "Enabled")) { g_enabled.store(value != 0); save_float(L"Enabled", static_cast<float>(value)); }
    else if (!std::strcmp(key, "Triggers")) { g_triggers.store(value != 0); save_float(L"Triggers", static_cast<float>(value)); }
    else if (!std::strcmp(key, "LightBar")) { g_lights.store(value != 0); save_float(L"LightBar", static_cast<float>(value)); }
    else if (!std::strcmp(key, "Bluetooth")) { g_bluetooth.store(value != 0); save_float(L"Bluetooth", static_cast<float>(value)); }
    else if (!std::strcmp(key, "RevLights")) { g_rev_lights.store(value != 0); save_float(L"RevLights", static_cast<float>(value)); }
}
void on_slide(void*, const char* key, float value) {
    if (!std::strcmp(key, "Rumble")) { g_rumble.store(value); save_float(L"Rumble", value); }
    else if (!std::strcmp(key, "TriggerStrength")) { g_trig_strength.store(value); save_float(L"TriggerStrength", value); }
}

// ---- the player's car, from the game thread -----------------------------------------------------------------------------------
std::atomic<void*> g_hud_player{nullptr};
std::atomic<ULONGLONG> g_hud_tick{0};
std::atomic<ULONGLONG> g_car_tick{0};
std::atomic<float> g_ratio{0};        // engine speed / its limit
std::atomic<float> g_throttle{0};
std::atomic<float> g_speed{0};        // m/s
std::atomic<int> g_gear{0};
std::atomic<ULONGLONG> g_shift_tick{0};
std::atomic<ULONGLONG> g_impact_tick{0};
std::atomic<float> g_impact_amp{0};

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

double now_s() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

float g_prev_v[3] = {0, 0, 0};
double g_prev_t   = 0;
bool g_prev_ok    = false;
int g_prev_gear   = -1;

void sample(char* car) {
    __try {
        char* iface = *reinterpret_cast<char**>(car + kOffIface);
        if (!iface) return;
        char* dyn = iface - kIfaceToDyn;
        const float omega = *reinterpret_cast<const float*>(iface + kIfOmega);
        const int gear    = *reinterpret_cast<const uint8_t*>(iface + kIfGear) & 0x0F;
        float limit       = *reinterpret_cast<const float*>(dyn + kDynEngMax);
        if (!std::isfinite(limit) || limit < 200.0f || limit > 1500.0f) limit = 7000.0f / kRadPerSecToRpm;
        const float thr = *reinterpret_cast<const float*>(dyn + kDynThrRaw);
        const float* v  = reinterpret_cast<const float*>(dyn + kDynVelocity);
        const float vx = v[0], vy = v[1], vz = v[2];
        if (!std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz) || !std::isfinite(omega)) return;
        const double t = now_s();
        g_ratio.store(clampf(omega / limit, 0, 1.2f));
        g_throttle.store(std::isfinite(thr) ? clampf(thr, 0, 1) : 0);
        g_speed.store(std::sqrt(vx * vx + vy * vy + vz * vz));
        g_gear.store(gear);
        if (g_prev_gear > 0 && gear > 0 && gear != g_prev_gear) g_shift_tick.store(GetTickCount64());
        g_prev_gear = gear;
        if (g_prev_ok) {
            const double dt = t - g_prev_t;
            if (dt > 0.004 && dt < 0.1) {
                const float dx = vx - g_prev_v[0], dy = vy - g_prev_v[1], dz = vz - g_prev_v[2];
                const float a  = std::sqrt(dx * dx + dy * dy + dz * dz) / static_cast<float>(dt);   // m/s^2
                if (a > 40.0f) {   // braking is up to ~14, a wall or a landing is far more
                    const float amp = clampf((a - 40.0f) / 220.0f, 0.3f, 1.0f);
                    if (amp > g_impact_amp.load() || GetTickCount64() - g_impact_tick.load() > 250) g_impact_amp.store(amp);
                    g_impact_tick.store(GetTickCount64());
                }
            }
        }
        g_prev_v[0] = vx; g_prev_v[1] = vy; g_prev_v[2] = vz;
        g_prev_t  = t;
        g_prev_ok = true;
        g_car_tick.store(GetTickCount64());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

typedef uint64_t (*CarUpdateFn)(void*, float);
CarUpdateFn g_orig_car_update = nullptr;
uint64_t detour_car_update(void* self, float dt) {
    const uint64_t r = g_orig_car_update(self, dt);
    char* car        = static_cast<char*>(self);
    void* hud        = g_hud_player.load();
    if (!car || !hud) return r;
    __try {
        void* iface = *reinterpret_cast<void**>(car + kOffIface);
        if (static_cast<void*>(car) == hud || (iface && iface == hud)) sample(car);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return r;
}

typedef uint64_t (*HudCtrlFn)(void*, void*, void*, void*);
HudCtrlFn g_orig_hud = nullptr;
uint64_t detour_hud(void* c, void* a, void* b, void* d) {
    const uint64_t r = g_orig_hud(c, a, b, d);
    if (c && readable(c, 0x130)) {
        void* obj = *reinterpret_cast<void**>(static_cast<char*>(c) + 0x120);
        void* cb  = *reinterpret_cast<void**>(static_cast<char*>(c) + 0x128);
        g_hud_player.store(cb ? obj : nullptr);
        g_hud_tick.store(GetTickCount64());
    }
    return r;
}

DWORD WINAPI install_hooks(LPVOID) {
    g_game = static_cast<char*>(g_api->wait_for_code(0xFFFFFFFFu));
    if (!g_game) { say("the game's code never became ready: no hooks"); return 0; }
    FH3Hook* h1 = g_api->hook_function(g_mod, "Car update (DualSense)", g_game + kRvaCarUpdate, reinterpret_cast<void*>(&detour_car_update),
                                       reinterpret_cast<void**>(&g_orig_car_update));
    FH3Hook* h2 = g_api->hook_function(g_mod, "HUD controller (DualSense: the player's car)", g_game + kRvaHudCtrl, reinterpret_cast<void*>(&detour_hud),
                                       reinterpret_cast<void**>(&g_orig_hud));
    const bool a = h1 && g_api->enable_hooks(g_mod, h1);
    const bool b = h2 && g_api->enable_hooks(g_mod, h2);
    say("hooks: Car update %s, HUD controller %s", a ? "on" : "FAILED", b ? "on" : "FAILED");
    return 0;
}

// ---- the controller (USB) -------------------------------------------------------------------------------------------------------
constexpr USHORT kVid = 0x054C, kPid = 0x0CE6, kPidEdge = 0x0DF2;

struct Ds {
    HANDLE h = nullptr;
    USHORT outLen = 0, featLen = 0;
    bool v2 = false;
    bool bt = false;
    BYTE seq = 0;
    OVERLAPPED wr{};
};
Ds g_ds;

bool contains_ci(const wchar_t* s, const wchar_t* what) {
    const int n = lstrlenW(what);
    for (; *s; ++s)
        if (CompareStringOrdinal(s, n, what, n, TRUE) == CSTR_EQUAL) return true;
    return false;
}

bool ds_open(Ds& d) {
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO set = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof ifd;
    bool found = false;
    static char last_why[160] = "?";
    char why[160]             = "no DualSense connected";
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(set, nullptr, &hid, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        auto* det = static_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(HeapAlloc(GetProcessHeap(), 0, need + 16));
        if (!det) continue;
        det->cbSize = sizeof *det;
        if (SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr) && contains_ci(det->DevicePath, L"054c")) {
            HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                   FILE_FLAG_OVERLAPPED, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                std::snprintf(why, sizeof why, "a Sony HID device doesn't open (error %lu): another program has it?", GetLastError());
            } else {
                HIDD_ATTRIBUTES a{};
                a.Size = sizeof a;
                PHIDP_PREPARSED_DATA pp = nullptr;
                bool ok                 = false;
                if (HidD_GetAttributes(h, &a) && a.VendorID == kVid && (a.ProductID == kPid || a.ProductID == kPidEdge) && HidD_GetPreparsedData(h, &pp)) {
                    HIDP_CAPS caps{};
                    if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 1 && caps.Usage == 5) {
                        const bool bt = caps.InputReportByteLength != 64;
                        if (caps.OutputReportByteLength < (bt ? 78 : 48)) {
                            std::snprintf(why, sizeof why, "the DualSense's output report is %u bytes: not supported", caps.OutputReportByteLength);
                        } else if (bt && !g_bluetooth.load()) {
                            std::snprintf(why, sizeof why, "the DualSense is on Bluetooth and Bluetooth effects are off (DualSense.ini Bluetooth=0)");
                        } else {
                            d.h       = h;
                            d.bt      = bt;
                            d.seq     = 0;
                            d.outLen  = caps.OutputReportByteLength;
                            d.featLen = caps.FeatureReportByteLength;
                            ok = found = true;
                        }
                    }
                    HidD_FreePreparsedData(pp);
                }
                if (!ok) CloseHandle(h);
            }
        }
        HeapFree(GetProcessHeap(), 0, det);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (!found) {
        if (std::strcmp(why, last_why)) {
            std::snprintf(last_why, sizeof last_why, "%s", why);
            say("controller: %s -- looking again every 2 seconds", why);
        }
        return false;
    }
    std::snprintf(last_why, sizeof last_why, "?");
    d.wr.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    d.v2        = true;   // modern firmware unless the report below says otherwise
    if (d.featLen && d.featLen <= 600) {   // the firmware report: rumble emulation v2 from update 0x0224 on
        BYTE feat[600] = {};
        feat[0]        = 0x20;
        if (HidD_GetFeature(d.h, feat, d.featLen)) {
            const unsigned update = static_cast<unsigned>(feat[44]) | (static_cast<unsigned>(feat[45]) << 8);
            d.v2                  = update >= 0x0224;
            say("controller: DualSense over %s (firmware update %04X, rumble emulation v%d)", d.bt ? "Bluetooth" : "USB", update, d.v2 ? 2 : 1);
        } else {
            say("controller: DualSense over %s", d.bt ? "Bluetooth" : "USB");
        }
    } else {
        say("controller: DualSense over %s", d.bt ? "Bluetooth" : "USB");
    }
    return true;
}

void ds_close(Ds& d) {
    if (!d.h) return;
    CancelIo(d.h);
    CloseHandle(d.h);
    if (d.wr.hEvent) CloseHandle(d.wr.hEvent);
    d.wr = OVERLAPPED{};
    d.h  = nullptr;
}

uint32_t g_crc_table[256];
void crc_init() {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        g_crc_table[i] = c;
    }
}
uint32_t crc32(uint32_t crc, const BYTE* p, size_t n) {
    crc = ~crc;
    while (n--) crc = g_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool ds_write(Ds& d, const BYTE common[47]) {
    BYTE buf[128] = {};
    if (d.outLen > sizeof buf) return false;
    if (!d.bt) {
        buf[0] = 0x02;
        std::memcpy(buf + 1, common, 47);
    } else {   // Bluetooth: report 0x31, a sequence, a tag, the same 47 bytes, a CRC (seed 0xA2 for outputs)
        const BYTE seed = 0xA2;
        buf[0] = 0x31;
        buf[1] = static_cast<BYTE>((d.seq++ & 0x0F) << 4);
        buf[2] = 0x10;
        std::memcpy(buf + 3, common, 47);
        const uint32_t crc = crc32(crc32(0, &seed, 1), buf, 74);
        buf[74] = static_cast<BYTE>(crc);
        buf[75] = static_cast<BYTE>(crc >> 8);
        buf[76] = static_cast<BYTE>(crc >> 16);
        buf[77] = static_cast<BYTE>(crc >> 24);
    }
    ResetEvent(d.wr.hEvent);
    DWORD n = 0;
    if (!WriteFile(d.h, buf, d.outLen, nullptr, &d.wr)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(d.wr.hEvent, 200) != WAIT_OBJECT_0) {
            CancelIoEx(d.h, &d.wr);
            GetOverlappedResult(d.h, &d.wr, &n, TRUE);
            return false;
        }
    }
    return GetOverlappedResult(d.h, &d.wr, &n, FALSE) != 0;
}

// Trigger effects (11 bytes; Nielk1's encoding, as the PS5 uses).
void trig_off(BYTE* e) { std::memset(e, 0, 11); e[0] = 0x05; }
void trig_zones(BYTE* e, BYTE mode, const BYTE force[10], BYTE freq) {
    uint32_t zones = 0;
    uint16_t active = 0;
    std::memset(e, 0, 11);
    for (int i = 0; i < 10; ++i)
        if (force[i]) {
            zones |= static_cast<uint32_t>((force[i] - 1) & 7) << (3 * i);
            active |= static_cast<uint16_t>(1 << i);
        }
    if (!active) { trig_off(e); return; }
    e[0] = mode;
    e[1] = static_cast<BYTE>(active);
    e[2] = static_cast<BYTE>(active >> 8);
    e[3] = static_cast<BYTE>(zones);
    e[4] = static_cast<BYTE>(zones >> 8);
    e[5] = static_cast<BYTE>(zones >> 16);
    e[6] = static_cast<BYTE>(zones >> 24);
    e[9] = freq;
}
int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
// A brake pedal: light first, harder towards the end of the travel.
void trig_brake(BYTE* e) {
    BYTE force[10] = {};
    for (int i = 2; i < 10; ++i) force[i] = static_cast<BYTE>(clampi(1 + (i - 2) * 5 / 7, 1, 8));
    trig_zones(e, 0x21, force, 0);
}
void trig_vib(BYTE* e, int amp, int hz) {
    BYTE a[10];
    for (int i = 0; i < 10; ++i) a[i] = static_cast<BYTE>(clampi(amp, 1, 8));
    trig_zones(e, 0x26, a, static_cast<BYTE>(clampi(hz, 1, 255)));
}

// The game's own rumble, which the platform gets from the engine when it hands the controller to the game as a gamepad
// (shared memory "Local\FH3ModPlatform.Vibration": left motor, right motor, left trigger, right trigger, tick count).
struct GameVib { float lm, rm, lt, rt; DWORD tick; };
const GameVib* game_vib() {
    static const GameVib* p = nullptr;
    if (!p) {
        if (HANDLE m = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\FH3ModPlatform.Vibration"))
            p = static_cast<const GameVib*>(MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(GameVib)));
    }
    return p;
}

bool game_in_front() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

BYTE mixb(int a, int b, float t) { return static_cast<BYTE>(a + (b - a) * clampf(t, 0, 1)); }

struct Last {
    BYTE trig[2][11] = {};
    BYTE rgb[3]      = {};
    BYTE player      = 0xFF;
    ULONGLONG full   = 0;
    BYTE report[47]  = {};
    ULONGLONG write  = 0;
    bool first       = true;
} g_last;

void send_output(Ds& d, bool neutral) {
    const ULONGLONG now = GetTickCount64();
    const bool driving  = !neutral && g_enabled.load() && now - g_hud_tick.load() < 600 && now - g_car_tick.load() < 400;
    const float ratio   = driving ? g_ratio.load() : 0;
    const float thr     = driving ? g_throttle.load() : 0;
    const float strength = g_rumble.load();
    const bool full     = g_last.first || now - g_last.full > 1000;

    BYTE c[47] = {};
    // rumble emulation
    float heavy = 0, light = 0;
    if (driving && strength > 0) {
        if (ratio > 0.02f) heavy = (0.10f + 0.20f * ratio) * (0.4f + 0.6f * thr);
        const bool limiter = ratio >= 0.975f && thr > 0.5f;
        if (limiter && ((now / 30) & 1)) light = 0.35f;
        const ULONGLONG ds = now - g_shift_tick.load();
        if (g_shift_tick.load() && ds < 110) light = std::max(light, 0.55f * (1.0f - static_cast<float>(ds) / 110.0f));
        const ULONGLONG di = now - g_impact_tick.load();
        if (g_impact_tick.load() && di < 260) {
            const float k = g_impact_amp.load() * (1.0f - static_cast<float>(di) / 260.0f);
            heavy = std::max(heavy, k);
            light = std::max(light, k * 0.8f);
        }
        heavy *= strength;
        light *= strength;
    }
    if (!neutral && g_enabled.load() && strength > 0) {   // the game's own rumble, when the platform gave it the controller
        if (const GameVib* gv = game_vib()) {
            if (static_cast<DWORD>(GetTickCount() - gv->tick) < 500) {
                heavy = std::max(heavy, clampf(gv->lm, 0, 1) * strength);
                light = std::max(light, clampf(gv->rm, 0, 1) * strength);
            }
        }
    }
    c[0] |= 0x02;
    if (d.v2) c[38] |= 0x04; else c[0] |= 0x01;
    c[2] = static_cast<BYTE>(clampf(light, 0, 1) * 255.0f);
    c[3] = static_cast<BYTE>(clampf(heavy, 0, 1) * 255.0f);

    // triggers
    BYTE e[2][11];   // [0] left, [1] right
    trig_off(e[0]);
    trig_off(e[1]);
    if (driving && g_triggers.load()) {
        trig_brake(e[0]);
        if (ratio >= 0.975f && thr > 0.5f) trig_vib(e[1], 5, 40);
    }
    if (full || std::memcmp(e, g_last.trig, sizeof e)) {
        c[0] |= 0x04 | 0x08;
        std::memcpy(c + 10, e[1], 11);
        std::memcpy(c + 21, e[0], 11);
        std::memcpy(g_last.trig, e, sizeof e);
        c[36] = static_cast<BYTE>(clampi(static_cast<int>((1.0f - g_trig_strength.load()) * 8), 0, 7));
        c[1] |= 0x40;
    }

    // light bar and player lights
    BYTE rgb[3] = {static_cast<BYTE>(g_color[0]), static_cast<BYTE>(g_color[1]), static_cast<BYTE>(g_color[2])};
    if (driving) {
        const float t = clampf((ratio - 0.55f) / 0.4f, 0, 1);
        rgb[0] = mixb(g_color[0], 255, t);
        rgb[1] = mixb(g_color[1], 0, t);
        rgb[2] = mixb(g_color[2], 0, t);
        if (ratio >= 0.975f && ((now / 40) & 1)) rgb[0] = rgb[1] = rgb[2] = 0;
    }
    if (g_last.first) {
        c[38] |= 0x02;   // light bar setup: ends the blue start-up animation
        c[41] = 0x02;
    }
    if (g_lights.load() && (full || std::memcmp(rgb, g_last.rgb, 3))) {
        c[1] |= 0x04;
        c[44] = rgb[0]; c[45] = rgb[1]; c[46] = rgb[2];
        std::memcpy(g_last.rgb, rgb, 3);
    }
    static const BYTE bars[6] = {0x00, 0x01, 0x03, 0x07, 0x0F, 0x1F};
    BYTE player = 0x04;   // player 1
    if (driving && g_rev_lights.load()) {
        if (ratio >= 0.975f) player = ((now / 50) & 1) ? 0x1F : 0x00;
        else player = ratio < 0.70f ? 0x00 : bars[clampi(static_cast<int>((ratio - 0.70f) / 0.055f) + 1, 1, 5)];
    }
    if (g_lights.load() && (full || player != g_last.player)) {
        c[1] |= 0x10;
        c[43]         = player;
        g_last.player = player;
    }

    if (!full && !g_last.first && !std::memcmp(c, g_last.report, sizeof c) && now - g_last.write < 250) return;
    if (ds_write(d, c)) {
        g_last.first = false;
        std::memcpy(g_last.report, c, sizeof c);
        g_last.write = now;
        if (full) g_last.full = now;
    } else {
        say("controller: gone (write error %lu)", GetLastError());
        ds_close(d);
    }
}

// The controller back to itself: no rumble, no trigger effects, its own lights.
void send_reset(Ds& d) {
    if (!d.h) return;
    BYTE c[47] = {};
    c[0] = 0x02 | 0x01 | 0x04 | 0x08;
    trig_off(c + 10);
    trig_off(c + 21);
    c[1] = 0x08 | 0x01;   // release the LEDs to the controller
    ds_write(d, c);
}

DWORD WINAPI controller_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    ULONGLONG next_scan = 0;
    bool was_neutral    = true;
    for (;;) {
        Sleep(10);
        const ULONGLONG now = GetTickCount64();
        if (!g_ds.h) {
            if (now < next_scan) continue;
            next_scan = now + 2000;
            if (!g_enabled.load() || !ds_open(g_ds)) continue;
            g_last = Last{};
            was_neutral = true;
        }
        const bool neutral = !g_enabled.load() || !game_in_front();
        if (neutral && !was_neutral) {   // switched off, or another window in front: the controller is itself again
            send_reset(g_ds);
            g_last = Last{};
        }
        was_neutral = neutral;
        if (!neutral) send_output(g_ds, false);
    }
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH && g_ds.h) send_reset(g_ds);   // the game ends: the controller back to itself
    return TRUE;
}

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api) {
    if (api->api_version < 11) return 1;
    g_api = api;
    FH3ModInfo info{};
    info.size             = sizeof info;
    info.name             = "DualSense";
    info.version          = DUALSENSE_VERSION;
    info.github_repo      = nullptr;
    info.platform_version = FH3MOD_PLATFORM_VERSION;
    info.check_updates    = 0;
    info.description      = "DualSense (USB and Bluetooth): rev-counter light bar and player lights, brake-pedal trigger, rev-limiter trigger vibration, engine hum, gear-change and impact rumble.";
    info.offline_only     = 1;
    g_mod                 = api->register_mod(&info);
    if (!g_mod) return 1;

    g_cfg_path = std::wstring(api->azimuth_dir) + L"\\fh3-mods\\DualSense.ini";
    load_config();
    crc_init();

    auto toggle = [&](const char* key, const char* label, const char* desc, bool on) {
        FH3Setting s{};
        s.size        = sizeof s;
        s.key         = key;
        s.label       = label;
        s.description = desc;
        s.value       = on ? 1 : 0;
        s.on_change   = on_toggle;
        api->add_setting(g_mod, &s);
    };
    auto slider = [&](const char* key, const char* label, const char* desc, float v, float mx) {
        FH3Setting s{};
        s.size          = sizeof s;
        s.key           = key;
        s.label         = label;
        s.description   = desc;
        s.kind          = FH3_SETTING_SLIDER;
        s.min           = 0;
        s.max           = mx;
        s.step          = 0.1f;
        s.slider_value  = v;
        s.display_scale = 100;
        s.decimals      = 0;
        s.on_slide      = on_slide;
        api->add_setting(g_mod, &s);
    };
    toggle("Enabled", "DualSense effects", "The DualSense's lights, triggers and rumble follow the car. Off gives the controller back to itself.", g_enabled.load());
    slider("Rumble", "Rumble strength (%)", "The engine's hum, gear-change knocks and impacts in the controller's motors. 0 turns them off.", g_rumble.load(), 2.0f);
    toggle("Triggers", "Adaptive triggers", "L2 as a brake pedal, R2 vibrating on the rev limiter.", g_triggers.load());
    slider("TriggerStrength", "Trigger strength (%)", "How hard the trigger effects push.", g_trig_strength.load(), 1.0f);
    toggle("Bluetooth", "Bluetooth effects", "Also over Bluetooth. The controller is switched to its full reports (the platform reads them for the mods). Turn off if the game loses the controller. Takes effect when the controller reconnects.", g_bluetooth.load());
    toggle("LightBar", "Lights", "The light bar goes red towards the redline.", g_lights.load());
    toggle("RevLights", "Rev counter on the player lights", "The five player lights count the revs.", g_rev_lights.load());

    if (HANDLE t = CreateThread(nullptr, 0, install_hooks, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, controller_thread, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}
