// DualSense for Forza Horizon 3 (FH3 Mod Platform mod) -- the feel of FH2-Input, on FH3, over USB and Bluetooth alike.
//
// The car, from the game's own controller feedback code (the same two functions FH2-Input hooks in FH2; in FH3 at
// game+0x1A20BF0 (rumble: the car's "CarIn": revs 0..1 at the redline, collision impulses with cars / the world / objects, tyre
// slip) and game+0x1A21410 (haptics: the wheels' spin and brake slip, 1 = the grip limit)), plus Car::Update (game+0x18B8D20) for
// the player's car's gear, speed and the surface under each wheel (its dynamics interface's own wheel-contact call, as the game's
// code does it: Car+0x71E8, vtable +0x5C8; the surfaces' table at game+0x4E72CD0, records of 0x3C0 bytes).
//
//  - HD haptics: the engine's hum at its revs, the road under each side (gravel, dirt, grass, sand, snow, cobbles, kerbs, metal,
//    bumpy tarmac), crashes, landings and take-offs, gear-change knocks, the rev limiter, the game's own rumble as two tones, menu
//    ticks and clicks. USB: the controller's audio endpoint (channels 3 and 4). Bluetooth: report 0x32 (8-bit stereo at 3 kHz,
//    32 frames a report; SAxense by Sdore, https://apps.sdore.me/SAxense). "Rumble" falls back to the motors' emulation.
//  - Adaptive triggers: L2 a brake pedal; ABS pulses at the grip limit, or hard where it is when a wheel locks without ABS (learnt
//    from the brakes, as FH2-Input does); R2 vibrates with wheel spin and knocks on the rev limiter; gear changes knock the trigger
//    pressed; crashes and landings jolt both; the road's texture in the trigger pressed.
//  - Lights: the light bar (colour, impacts, battery or revs), player lights as a rev counter, the battery for 3 s on the PS
//    button, a low battery blinking, the mic light for gyro steering (the platform's).
// The controller's input (buttons, sticks, gyro, touchpad, keyboard, mouse) is the platform's (1.1.0): the game gets the pad from it.
// Nothing is written into the game's folder; the controller goes back to itself when the game ends or loses focus.
#include "fh3mod.h"

#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <mmreg.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace {

// ---- the game ---------------------------------------------------------------------------------------------------------------------
constexpr uintptr_t kRvaCarUpdate = 0x18B8D20;
constexpr uintptr_t kRvaHudCtrl   = 0xB26690;
constexpr uintptr_t kRvaRumble    = 0x1A20BF0;
constexpr uintptr_t kRvaHaptics   = 0x1A21410;
constexpr uintptr_t kRvaSurfTable = 0x4E72CD0;
const uint8_t kRumbleProlog[]  = {0x48, 0x8B, 0xC4, 0x56, 0x57, 0x48, 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00, 0x80, 0x79, 0x0C, 0x00, 0x49, 0x8B, 0xF8};
const uint8_t kHapticsProlog[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x83, 0xC1, 0x10};
constexpr size_t kOffIface    = 0x71E8;
constexpr size_t kIfaceToDyn  = 0x4B0;
constexpr size_t kDynVelocity = 0x10;
constexpr size_t kIfGear      = 0xAA8;
constexpr size_t kDynEngOmega = 0x5F8;
constexpr size_t kDynEngMax   = 0x684;
constexpr size_t kDynThrRaw   = 0x1748;
constexpr size_t kVfnWheelContact = 0x5C8;
constexpr size_t kSurfStride  = 0x3C0;

const FH3ModApi* g_api = nullptr;
FH3Mod* g_mod          = nullptr;
char* g_game           = nullptr;
std::wstring g_cfg_path;

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
double now_s() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}
bool game_in_front() {
    HWND w    = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

// ---- settings: fh3-mods\DualSense.ini [DualSense] (the platform keeps [Controller], [Keyboard], [Keys], [Mouse] in it) -----------
struct Settings {
    int enabled = 1, bluetooth = 1;
    int vibration = 2;          // 0 off, 1 the motors' rumble emulation, 2 HD haptics (USB: audio; Bluetooth: report 0x32)
    float strength = 1.0f;      // everything felt, 0..2
    float car = 0.4f;           // the engine's hum, the limiter, gear changes
    float collisions = 0.6f, surface = 0.6f, events = 0.7f, menus = 0.6f;
    float low_hz = 45, high_hz = 160;   // the game's rumble as tones: the heavy (left) and light (right) motor
    int triggers = 2;           // 0 off, 1 resistance only, 2 the car's feedback too
    float trig_strength = 0.8f;
    int brake_resist = 2, brake_start = 0, brake_f0 = 2, brake_f1 = 8;     // 0 none, 1 constant, 2 a slope
    int throttle_resist = 1, throttle_start = 1, throttle_f = 1;
    int brake_hz = 34, throttle_hz = 55;
    float brake_from = 1.5f, spin_from = 0.8f;
    int free_in_menus = 1, crash_jolt = 1, gear_kick = 1, rev_limiter = 1;
    int light_mode = 4;         // 0 off, 1 colour, 2 impacts, 3 battery, 4 revs
    float brightness = 1.0f;
    int color[3] = {0, 90, 255}, flash[3] = {255, 255, 255};
    int rev_lights = 1, ps_battery = 1, low_battery_blink = 1;
    int log = 0;
} S;

bool is_problem(const char* s) {
    static const char* const words[] = {"could not", "couldn't", "can't", "cannot", "failed", "not found", "error", "not as expected"};
    char low[256];
    size_t i = 0;
    for (; s[i] && i < sizeof low - 1; ++i) low[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    low[i] = 0;
    for (const char* w : words)
        if (std::strstr(low, w)) return true;
    return false;
}
void say(const char* fmt, ...) {
    char line[700];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_api && g_mod && (S.log || is_problem(line) || std::strncmp(line, "controller", 10) == 0 || std::strncmp(line, "haptics", 7) == 0 ||
                           std::strncmp(line, "car:", 4) == 0 || std::strncmp(line, "brakes", 6) == 0 || std::strncmp(line, "surface", 7) == 0))
        g_api->log(g_mod, line);
}
void vsay(const char* fmt, ...) {   // only with Log=1
    if (!S.log) return;
    char line[700];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (g_api && g_mod) g_api->log(g_mod, line);
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
int cfg_int(const wchar_t* key, int def) { return static_cast<int>(std::lround(cfg_float(key, static_cast<float>(def)))); }
void save(const wchar_t* key, float v) {
    wchar_t s[48];
    _snwprintf(s, 48, L"%g", static_cast<double>(v));
    WritePrivateProfileStringW(L"DualSense", key, s, g_cfg_path.c_str());
}

void load_config() {
    S.enabled       = cfg_int(L"Enabled", 1);
    S.bluetooth     = cfg_int(L"Bluetooth", 1);
    S.vibration     = clampi(cfg_int(L"Vibration", 2), 0, 2);
    S.strength      = clampf(cfg_float(L"Strength", 1.0f), 0, 2);
    S.car           = clampf(cfg_float(L"EngineFeel", 0.4f), 0, 1);
    S.collisions    = clampf(cfg_float(L"Collisions", 0.6f), 0, 1);
    S.surface       = clampf(cfg_float(L"RoadSurface", 0.6f), 0, 1);
    S.events        = clampf(cfg_float(L"Events", 0.7f), 0, 1);
    S.menus         = clampf(cfg_float(L"MenuHaptics", 0.6f), 0, 1);
    S.low_hz        = clampf(cfg_float(L"HapticsLowHz", 45), 10, 400);
    S.high_hz       = clampf(cfg_float(L"HapticsHighHz", 160), 10, 1000);
    S.triggers      = clampi(cfg_int(L"Triggers", 2), 0, 2);
    S.trig_strength = clampf(cfg_float(L"TriggerStrength", 0.8f), 0, 1);
    S.brake_resist  = clampi(cfg_int(L"BrakeResistance", 2), 0, 2);
    S.brake_start   = clampi(cfg_int(L"BrakePedalStart", 0), 0, 9);
    S.brake_f0      = clampi(cfg_int(L"BrakePedalForceStart", 2), 1, 8);
    S.brake_f1      = clampi(cfg_int(L"BrakePedalForceEnd", 8), 1, 8);
    S.throttle_resist = clampi(cfg_int(L"ThrottleResistance", 1), 0, 2);
    S.throttle_start  = clampi(cfg_int(L"ThrottleStart", 1), 0, 9);
    S.throttle_f      = clampi(cfg_int(L"ThrottleForce", 1), 1, 8);
    S.brake_hz      = clampi(cfg_int(L"BrakeHz", 34), 1, 255);
    S.throttle_hz   = clampi(cfg_int(L"ThrottleHz", 55), 1, 255);
    S.brake_from    = clampf(cfg_float(L"BrakeSlipFrom", 1.5f), 0.5f, 5);
    S.spin_from     = clampf(cfg_float(L"SpinSlipFrom", 0.8f), 0.2f, 5);
    S.free_in_menus = cfg_int(L"FreeTriggersInMenus", 1);
    S.crash_jolt    = cfg_int(L"CrashJolt", 1);
    S.gear_kick     = cfg_int(L"GearKick", 1);
    S.rev_limiter   = cfg_int(L"RevLimiter", 1);
    S.light_mode    = clampi(cfg_int(L"LightBar", 4), 0, 4);
    S.brightness    = clampf(cfg_float(L"Brightness", 1.0f), 0, 1);
    S.color[0] = clampi(cfg_int(L"ColorR", 0), 0, 255);
    S.color[1] = clampi(cfg_int(L"ColorG", 90), 0, 255);
    S.color[2] = clampi(cfg_int(L"ColorB", 255), 0, 255);
    S.flash[0] = clampi(cfg_int(L"FlashR", 255), 0, 255);
    S.flash[1] = clampi(cfg_int(L"FlashG", 255), 0, 255);
    S.flash[2] = clampi(cfg_int(L"FlashB", 255), 0, 255);
    S.rev_lights        = cfg_int(L"RevLights", 1);
    S.ps_battery        = cfg_int(L"PSButtonBattery", 1);
    S.low_battery_blink = cfg_int(L"LowBatteryBlink", 1);
    S.log               = cfg_int(L"Log", 0);
}

// ---- shared with the platform: "Local\FH3ModPlatform.Vibration" --------------------------------------------------------------------
struct Shared {
    float lm, rm, lt, rt;   // the game's rumble (the engine's put_Vibration on the platform's gamepad)
    DWORD tick;
    volatile DWORD driving_tick;   // written here: the car's feedback came in (the platform's mouse knows the menus by it)
    volatile DWORD gyro_on;        // the platform's gyro steering (the mic light)
    DWORD reserved[9];
};
Shared* shared() {
    static Shared* p = nullptr;
    if (!p)
        if (HANDLE m = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, L"Local\\FH3ModPlatform.Vibration"))
            p = static_cast<Shared*>(MapViewOfFile(m, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(Shared)));
    return p;
}

// ---- the car ------------------------------------------------------------------------------------------------------------------------
enum { SURF_NONE = -1, SURF_TARMAC, SURF_KERB, SURF_GRAVEL, SURF_DIRT, SURF_GRASS, SURF_SAND, SURF_SNOW, SURF_COBBLE, SURF_METAL, SURF_BUMPY, SURF_KINDS };
const char* const kSurfName[SURF_KINDS] = {"tarmac", "kerb", "gravel", "dirt", "grass", "sand", "snow", "cobbles", "metal", "bumpy"};

struct CarState {
    volatile double at = 0;          // the car's feedback (the rumble function) last came in
    volatile float rpm = 0;          // 0 idle .. 1 redline (the real revs over the highest this car showed)
    volatile bool lim = false;       // on the rev limiter now
    float peak = 0, prev_rpm = 0; double lim_until = 0, rpm_at = 0;
    volatile float spin = 0, brake = 0;   // the haptics' slips, smoothed
    volatile double shift_at = 0, hit_at = 0;
    volatile float hit = 0;
    volatile bool hooked = false, haptics_hooked = false;
    // from Car::Update
    volatile float speed = 0, throttle = 0, eng_ratio = 0;
    volatile int gear = 0;
    volatile double update_at = 0;
} Car;

struct SurfState {
    volatile signed char cls[4] = {SURF_NONE, SURF_NONE, SURF_NONE, SURF_NONE};
    volatile float speed = 0;
    volatile double at = 0;
    bool air = false, lifted = false;
    double air_since = 0;
    volatile double land_at = 0;
    volatile float land = 0;
    volatile bool works = false;
} Surf;

struct Abs {
    float prev = 0, ds = 0, ext = 0, cnt = 0; int dir = 0; double act_until = 0;
    bool on = true;
    float lock_t = 0, limit_t = 0;
    bool locked_now = false;
    int clean_stops = 0;
} g_abs;

// the controller's input as this mod reads it (the triggers, the menus' haptics, the battery)
std::atomic<int> g_l2{0}, g_r2{0};
std::atomic<int> g_battery{0};
std::atomic<bool> g_charging{false};
std::atomic<double> g_battery_show_until{0};

bool car_driving(double now) { return Car.hooked && Car.at && now - Car.at < 0.35; }
float car_hit(double now) { return Car.hit_at ? Car.hit * std::exp(-static_cast<float>(now - Car.hit_at) / 0.09f) : 0; }
float hit_level(float v, float v0, float v1) { return v <= v0 ? 0 : std::min(std::log(v / v0) / std::log(v1 / v0), 1.0f); }

// ---- short haptic effects (FH2-Input's fx.h) ----------------------------------------------------------------------------------------
enum { FX_TICK, FX_THUMP, FX_BUZZ, FX_SWEEP, FX_NOISE };
struct FxVoice {
    int kind;
    double at;
    float len, amp, hz, hz1, decay, pan;
    uint32_t seed;
    bool used;
};
constexpr int FX_VOICES = 48;
FxVoice g_fx[FX_VOICES];
std::mutex g_fx_mu;
constexpr float TAU = 6.2831853f, PI_F = 3.14159265f;

float fx_hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du; x ^= x >> 15; x *= 0x846CA68Bu; x ^= x >> 16;
    return static_cast<float>(x & 0xFFFFFF) / 8388607.5f - 1.0f;
}
float fx_vnoise(double t, float rate, uint32_t seed) {
    const double p = t * rate, i = std::floor(p);
    float f = static_cast<float>(p - i);
    const uint32_t k = static_cast<uint32_t>(static_cast<int64_t>(i));
    f = f * f * (3 - 2 * f);
    return fx_hash(k * 2654435761u + seed) * (1 - f) + fx_hash((k + 1) * 2654435761u + seed) * f;
}
float fx_ping(float t, float hz, float decay) {
    if (t < 0 || t > decay * 8) return 0;
    return (t < 0.0004f ? t / 0.0004f : 1.0f) * std::exp(-t / decay) * std::sin(TAU * hz * t);
}
float fx_edge(float t, float len, float ramp) {
    float e = 1;
    if (t < ramp) e = t / ramp;
    if (len - t < ramp) e = std::min(e, (len - t) / ramp);
    return e < 0 ? 0 : e;
}
void fx_add(int kind, float amp, float hz, float hz1, float len, float decay, float pan = 0) {
    if (amp <= 0.003f) return;
    std::scoped_lock lk{g_fx_mu};
    int slot = 0;
    double oldest = 1e300;
    for (int i = 0; i < FX_VOICES; ++i) {
        if (!g_fx[i].used) { slot = i; oldest = -1; break; }
        if (g_fx[i].at < oldest) { oldest = g_fx[i].at; slot = i; }
    }
    static uint32_t seed = 0x9E3779B9u;
    seed = seed * 1664525u + 1013904223u;
    g_fx[slot] = FxVoice{kind, now_s(), len > 0 ? len : decay * 8, amp, hz, hz1, decay, pan, seed, true};
}
float fx_wave(const FxVoice& v, float t) {
    switch (v.kind) {
        case FX_TICK:
        case FX_THUMP: return fx_ping(t, v.hz, v.decay);
        case FX_BUZZ: {
            float s = std::sin(TAU * v.hz * t);
            if (v.hz1 > 0) s *= 0.55f + 0.45f * std::sin(TAU * v.hz1 * t);
            return s * fx_edge(t, v.len, 0.006f);
        }
        case FX_SWEEP: {
            const float ph = TAU * (v.hz * t + (v.hz1 - v.hz) * t * t / (2 * v.len));
            return std::sin(ph) * std::sin(PI_F * t / v.len);
        }
        case FX_NOISE: return fx_vnoise(t, v.hz, v.seed) * (t < 0.002f ? t / 0.002f : 1.0f) * std::exp(-t / v.decay);
    }
    return 0;
}
// into left/right (added), n frames from time t0, dt apart
void fx_render(float* left, float* right, int n, double t0, double dt) {
    FxVoice v[FX_VOICES];
    {
        std::scoped_lock lk{g_fx_mu};
        std::memcpy(v, g_fx, sizeof v);
    }
    for (int i = 0; i < FX_VOICES; ++i) {
        if (!v[i].used) continue;
        const float gl = v[i].amp * (v[i].pan > 0 ? 1 - v[i].pan : 1), gr = v[i].amp * (v[i].pan < 0 ? 1 + v[i].pan : 1);
        for (int f = 0; f < n; ++f) {
            const float t = static_cast<float>(t0 + f * dt - v[i].at);
            if (t < 0 || t > v[i].len) continue;
            const float s = fx_wave(v[i], t);
            left[f] += s * gl;
            right[f] += s * gr;
        }
    }
}
// the voices' envelopes for the motors: heavy (low tones) and light
void fx_rumble(double now, float* heavy, float* light) {
    std::scoped_lock lk{g_fx_mu};
    for (FxVoice& v : g_fx) {
        if (!v.used) continue;
        const float t = static_cast<float>(now - v.at);
        if (t > v.len + 0.05f) { v.used = false; continue; }
        if (t < 0) continue;
        float e = 0;
        switch (v.kind) {
            case FX_TICK: e = t < 0.035f ? 1.0f : 0; break;
            case FX_THUMP:
            case FX_NOISE: e = std::exp(-t / std::max(v.decay, 0.02f)); break;
            case FX_SWEEP: e = std::sin(PI_F * t / v.len); break;
            case FX_BUZZ: e = fx_edge(t, v.len, 0.01f); break;
        }
        e *= v.amp;
        if (v.hz < 100) *heavy = std::max(*heavy, e);
        else *light = std::max(*light, e);
    }
}
void fx_clear() {
    std::scoped_lock lk{g_fx_mu};
    for (FxVoice& v : g_fx) v.used = false;
}

// ---- the road (FH2-Input's surfaces) ----------------------------------------------------------------------------------------------
int surf_classify(const char* n) {
    auto is = [&](const char* p) { return std::strncmp(n, p, std::strlen(p)) == 0; };
    auto has = [&](const char* p) { return std::strstr(n, p) != nullptr; };
    if (is("RumbleStrip") || is("SpeedBump") || has("Rumble") || has("Kerb") || has("Curb")) return SURF_KERB;
    if (is("Gravel") || is("Rally_Gravel") || has("Gravel")) return SURF_GRAVEL;
    if (is("SandyDirt") || is("Dirt") || is("VineyardDirt") || is("Quicksand") || is("Rally_Dirt") || is("Rally_Ash") || has("Dirt") || has("Mud"))
        return SURF_DIRT;
    if (is("Sand") || has("Sand") || has("Beach")) return SURF_SAND;
    if (is("Grasscrete") || is("Latticecrete") || is("Cobblestone") || is("Brick") || has("Cobble")) return SURF_COBBLE;
    if (has("Grass") || is("Astroturf") || is("CornCrop") || has("Crop") || has("Field")) return SURF_GRASS;
    if (is("Snow") || has("Snow") || has("Ice")) return SURF_SNOW;
    if (is("MetalGrate") || is("MetalPlateBumpy") || has("Metal")) return SURF_METAL;
    if (is("BumpyAsphalt") || is("BumpyConcrete") || is("TarPatch") || has("Bumpy")) return SURF_BUMPY;
    return SURF_TARMAC;
}

// a surface record's name: a char array at its start, a std::string (MSVC) there, or a pointer to one
bool printable_name(const char* s, char* out, size_t cap) {
    size_t i = 0;
    for (; i < cap - 1 && s[i]; ++i)
        if (s[i] < 32 || s[i] > 126) return false;
    if (i < 2 || i >= cap - 1) return false;
    std::memcpy(out, s, i);
    out[i] = 0;
    return true;
}
bool record_name(const uint8_t* rec, char* out, size_t cap) {
    __try {
        if (printable_name(reinterpret_cast<const char*>(rec), out, cap)) return true;
        const uint64_t size = *reinterpret_cast<const uint64_t*>(rec + 16), capa = *reinterpret_cast<const uint64_t*>(rec + 24);
        if (size > 0 && size < 64 && capa >= size && capa < 4096) {   // std::string: the text in place below 16, else a pointer
            const char* p = capa < 16 ? reinterpret_cast<const char*>(rec) : *reinterpret_cast<const char* const*>(rec);
            if (printable_name(p, out, cap)) return true;
        }
        const char* p = *reinterpret_cast<const char* const*>(rec);
        if (reinterpret_cast<uintptr_t>(p) > 0x10000 && printable_name(p, out, cap)) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

int surf_kind(uint8_t idx) {
    static signed char kind[256];
    static const uint8_t* cached = nullptr;
    if (!g_game) return SURF_TARMAC;
    int k = SURF_TARMAC;
    __try {
        const uint8_t* t = *reinterpret_cast<const uint8_t* const*>(g_game + kRvaSurfTable);
        if (!t) return SURF_TARMAC;
        const uint8_t* b = *reinterpret_cast<const uint8_t* const*>(t + 8);
        const uint8_t* e = *reinterpret_cast<const uint8_t* const*>(t + 0x10);
        if (b != cached) {
            std::memset(kind, 0x7F, sizeof kind);
            cached = b;
        }
        if (kind[idx] != 0x7F) return kind[idx];
        if (b && e > b && static_cast<size_t>(e - b) / kSurfStride > idx) {
            char name[72];
            if (record_name(b + static_cast<size_t>(idx) * kSurfStride, name, sizeof name)) {
                k = surf_classify(name);
                say("surface %u: %s -> %s", idx, name, kSurfName[k]);
            } else {
                say("surface %u: no name found in its record -> tarmac", idx);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SURF_TARMAC;
    }
    kind[idx] = static_cast<signed char>(k);
    return k;
}
float surf_speed_level(float speed) { return speed < 1.5f ? 0 : std::pow(std::min((speed - 1.5f) / 23.5f, 1.0f), 0.6f); }
float surf_gain(int cls, float level, float speed) { return cls == SURF_KERB ? (0.4f + 0.6f * level) * std::min(speed / 3.0f, 1.0f) : level; }
int surf_side(int side) {
    const int a = Surf.cls[side], b = Surf.cls[side + 2];
    if (a == SURF_KERB || b == SURF_KERB) return SURF_KERB;
    return a >= 0 ? a : b;
}
int surf_main() {
    const int l = surf_side(0), r = surf_side(1);
    if (l == SURF_KERB || r == SURF_KERB) return SURF_KERB;
    return l > r ? l : r;
}

// the wheels this frame (the game's thread): their surfaces, a jump, a landing
using WheelContactFn = const uint8_t* (*)(void* iface, int wheel);
void surfaces(char* iface, float speed, double now) {
    int on = 0;
    signed char cls[4];
    __try {
        auto fn = reinterpret_cast<WheelContactFn>((*reinterpret_cast<void***>(iface))[kVfnWheelContact / 8]);
        for (int w = 0; w < 4; ++w) {
            const uint8_t* c = fn(iface, w);
            cls[w] = c ? static_cast<signed char>(surf_kind(c[1])) : static_cast<signed char>(SURF_NONE);
            on += c != nullptr;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int said = 0;
        if (!said++) say("surfaces: a fault reading the wheels' contacts: no road surfaces");
        Surf.works = false;
        return;
    }
    for (int w = 0; w < 4; ++w) Surf.cls[w] = cls[w];
    Surf.speed = speed;
    Surf.works = true;
    if (!on && speed > 3) {
        if (!Surf.air) { Surf.air = true; Surf.lifted = false; Surf.air_since = now; }
        else if (!Surf.lifted && now - Surf.air_since > 0.12) {   // a jump, not a crest
            Surf.lifted = true;
            fx_add(FX_SWEEP, 0.22f * S.events, 110, 45, 0.16f, 0);
        }
    } else if (on >= 2 && Surf.air) {
        const float t = static_cast<float>(now - Surf.air_since);
        Surf.air = false;
        if (t > 0.25f && t < 8) {   // a landing: the longer the flight, the harder
            const float k = std::min(0.3f + 0.3f * t, 1.0f) * (0.6f + 0.4f * surf_speed_level(speed));
            Surf.land = k;
            Surf.land_at = now;
            fx_add(FX_THUMP, 0.95f * k * S.events, 42, 0, 0, 0.11f);
            fx_add(FX_NOISE, 0.4f * k * S.events, 240, 0, 0, 0.03f);
        }
    } else if (on) {
        Surf.air = false;
    }
    Surf.at = now;
}

// ABS, as the brakes show it: raw = the front tyres' braking slip
void abs_learn(float raw, float dt) {
    const int l2 = g_l2.load();
    // ABS is working when the front slip keeps swinging up and down (the pump releasing and re-applying), not when it just sits high
    {
        g_abs.cnt = std::max(0.0f, g_abs.cnt - dt * 5.0f);
        g_abs.ds  = g_abs.ds * 0.5f + (raw - g_abs.prev) * 0.5f;
        g_abs.prev = raw;
        const int d = g_abs.ds > 0.01f ? 1 : (g_abs.ds < -0.01f ? -1 : 0);
        if (d && d != g_abs.dir) {
            if (g_abs.dir && std::fabs(raw - g_abs.ext) > 0.12f) g_abs.cnt += 1.0f;
            g_abs.dir = d;
            g_abs.ext = raw;
        }
        if (l2 > 90 && raw > 0.7f && raw < 3.6f && g_abs.cnt >= 2.5f) g_abs.act_until = now_s() + 0.18;
    }
    if (l2 > 150 && raw >= 3.6f) {
        g_abs.lock_t += dt;
        if (g_abs.lock_t > 0.15f) {
            g_abs.locked_now  = true;
            g_abs.clean_stops = 0;
            if (g_abs.on) {
                g_abs.on = false;
                say("brakes: a front wheel locked -- no ABS: the brake trigger shows lock-ups");
            }
        }
    } else {
        g_abs.lock_t = 0;
    }
    if (l2 > 200 && raw >= 1.1f && raw < 3.0f) g_abs.limit_t += dt;
    if (l2 < 60) {
        if (g_abs.limit_t > 0.8f && !g_abs.locked_now && !g_abs.on && ++g_abs.clean_stops >= 3) {
            g_abs.on = true;
            say("brakes: three hard stops at the grip limit without locking -- ABS: the brake trigger chatters");
        }
        g_abs.limit_t    = 0;
        g_abs.locked_now = false;
    }
}

// ---- hooks ---------------------------------------------------------------------------------------------------------------------------
std::atomic<void*> g_hud_player{nullptr};

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return static_cast<const char*>(p) + n <= static_cast<const char*>(mbi.BaseAddress) + mbi.RegionSize;
}

// the rumble function: (controller + 0x2F0, dt, CarIn*, flag). CarIn (as FH2's, longer): +0 revs 0..1, +8 car hits, +C world hits,
// +10 object hits, +14 slip, +18 front, +1C rear, +20 air, +24 bumpiness; bytes from +3C.
using RumbleFn = void (*)(void*, float, const uint8_t*, char);
RumbleFn g_orig_rumble = nullptr;
void detour_rumble(void* ctrl, float dt, const uint8_t* in, char flag) {
    if (in) {
        __try {
            const double now = now_s();
            const float* f   = reinterpret_cast<const float*>(in);
            const float rpm  = f[0];
            // f[1] is the engine's real rpm; f[0] stops near 0.92 at the limiter, so the revs are measured against the highest this car showed
            const float rr = f[1];
            if (std::isfinite(rr) && rr > 0 && rr < 30000) {
                const float thr = std::max(g_r2.load() / 255.0f, static_cast<float>(Car.throttle));
                if (Car.speed < 0.5f && thr < 0.15f && rr < Car.peak * 0.4f) Car.peak = rr;   // stood still at idle: a new car, learn again
                Car.peak = std::max(Car.peak, rr);
                const double dtr = Car.rpm_at ? now - Car.rpm_at : 0.016;
                if (dtr > 0.002) {
                    const float slope = std::fabs(rr - Car.prev_rpm) / static_cast<float>(dtr);
                    Car.prev_rpm = rr; Car.rpm_at = now;
                    if (Car.peak > 2500 && rr >= Car.peak * 0.985f && slope < 350.0f && thr > 0.5f) Car.lim_until = now + 0.12;
                }
                Car.lim = now < Car.lim_until;
                Car.rpm = Car.peak > 2500 ? clampf(rr / Car.peak, 0, 1.05f) : (std::isfinite(rpm) && rpm > -0.2f && rpm < 1.6f ? rpm : 0);
            } else if (std::isfinite(rpm) && rpm > -0.2f && rpm < 1.6f) {
                Car.rpm = rpm;
            }
            float hit = std::max(std::max(hit_level(f[2], 10, 400), hit_level(f[3], 25, 375)), hit_level(f[4], 0.8f, 30));
            hit = std::pow(hit, 1.3f);
            if (hit > 0.05f && hit > car_hit(now) * 1.15f) {
                Car.hit    = hit;
                Car.hit_at = now;
                if (hit > 0.15f) {
                    fx_add(FX_THUMP, 1.5f * hit * S.collisions, 55, 0, 0, 0.08f);
                    fx_add(FX_NOISE, 0.9f * hit * S.collisions, 320, 0, 0, 0.04f);
                }
            }
            if (!Car.at) say("car: the game's feedback data comes in (revs %.2f)", static_cast<double>(rpm));
            Car.at = now;
            if (Shared* sh = shared()) sh->driving_tick = GetTickCount();
            if (S.log) {
                static double last = 0;
                if (now - last > 0.7) {
                    last = now;
                    char b[600];
                    int n = std::snprintf(b, sizeof b, "car feedback:");
                    for (int i = 0; i < 15; ++i) n += std::snprintf(b + n, sizeof b - n, " %.3g", static_cast<double>(f[i]));
                    n += std::snprintf(b + n, sizeof b - n, " | bytes %02X %02X %02X %02X | spin %.2f brake %.2f | gear %d speed %.1f surf %d/%d/%d/%d",
                                       in[0x3C], in[0x3D], in[0x3E], in[0x3F], static_cast<double>(Car.spin), static_cast<double>(Car.brake), Car.gear,
                                       static_cast<double>(Car.speed), Surf.cls[0], Surf.cls[1], Surf.cls[2], Surf.cls[3]);
                    vsay("%s", b);
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_orig_rumble(ctrl, dt, in, flag);
}

float follow(float v, float target, float dt, float up, float down) {
    const float tau = target > v ? up : down;
    return v + (target - v) * (1 - std::exp(-dt / tau));
}
using HapticsFn = void (*)(void*, float, const float*);
HapticsFn g_orig_haptics = nullptr;
void detour_haptics(void* ctrl, float dt, const float* in) {
    if (in) {
        __try {
            const float d = dt > 0 && dt < 0.2f ? dt : 0.016f;
            if (std::isfinite(in[0]) && std::isfinite(in[1])) {
                Car.spin  = follow(Car.spin, in[0], d, 0.05f, 0.15f);
                Car.brake = follow(Car.brake, in[1], d, 0.05f, 0.15f);
                abs_learn(in[1], d);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    g_orig_haptics(ctrl, dt, in);
}

int g_prev_gear = -1;
void sample(char* car) {
    __try {
        char* iface = *reinterpret_cast<char**>(car + kOffIface);
        if (!iface) return;
        char* dyn       = iface - kIfaceToDyn;
        const int gear  = *reinterpret_cast<const uint8_t*>(iface + kIfGear) & 0x0F;
        const float* v  = reinterpret_cast<const float*>(dyn + kDynVelocity);
        const float spd = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        const float w   = *reinterpret_cast<const float*>(dyn + kDynEngOmega);
        const float lim = *reinterpret_cast<const float*>(dyn + kDynEngMax);
        const float thr = *reinterpret_cast<const float*>(dyn + kDynThrRaw);
        const double now = now_s();
        if (std::isfinite(spd)) Car.speed = spd;
        if (std::isfinite(thr)) Car.throttle = clampf(thr, 0, 1);
        if (std::isfinite(w) && std::isfinite(lim) && lim > 100) Car.eng_ratio = clampf(w / lim, 0, 1.2f);
        Car.gear = gear;
        if (g_prev_gear > 0 && gear > 0 && gear != g_prev_gear) {
            Car.shift_at = now;
            fx_add(FX_THUMP, 0.5f * S.car, 85, 0, 0, 0.03f);
        }
        g_prev_gear    = gear;
        Car.update_at  = now;
        surfaces(iface, std::isfinite(spd) ? spd : 0, now);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

using CarUpdateFn = uint64_t (*)(void*, float);
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

using HudCtrlFn = uint64_t (*)(void*, void*, void*, void*);
HudCtrlFn g_orig_hud = nullptr;
uint64_t detour_hud(void* c, void* a, void* b, void* d) {
    const uint64_t r = g_orig_hud(c, a, b, d);
    if (c && readable(c, 0x130)) {
        void* obj = *reinterpret_cast<void**>(static_cast<char*>(c) + 0x120);
        void* cb  = *reinterpret_cast<void**>(static_cast<char*>(c) + 0x128);
        g_hud_player.store(cb ? obj : nullptr);
    }
    return r;
}

DWORD WINAPI install_hooks(LPVOID) {
    g_game = static_cast<char*>(g_api->wait_for_code(0xFFFFFFFFu));
    if (!g_game) { say("the game's code never became ready: no hooks"); return 0; }
    bool ok_r = false, ok_h = false;
    if (!std::memcmp(g_game + kRvaRumble, kRumbleProlog, sizeof kRumbleProlog)) {
        FH3Hook* h = g_api->hook_function(g_mod, "car feedback: rumble (DualSense)", g_game + kRvaRumble, reinterpret_cast<void*>(&detour_rumble),
                                          reinterpret_cast<void**>(&g_orig_rumble));
        ok_r = h && g_api->enable_hooks(g_mod, h);
    }
    if (!std::memcmp(g_game + kRvaHaptics, kHapticsProlog, sizeof kHapticsProlog)) {
        FH3Hook* h = g_api->hook_function(g_mod, "car feedback: haptics (DualSense)", g_game + kRvaHaptics, reinterpret_cast<void*>(&detour_haptics),
                                          reinterpret_cast<void**>(&g_orig_haptics));
        ok_h = h && g_api->enable_hooks(g_mod, h);
    }
    Car.hooked         = ok_r;
    Car.haptics_hooked = ok_h;
    FH3Hook* h1 = g_api->hook_function(g_mod, "Car update (DualSense)", g_game + kRvaCarUpdate, reinterpret_cast<void*>(&detour_car_update),
                                       reinterpret_cast<void**>(&g_orig_car_update));
    FH3Hook* h2 = g_api->hook_function(g_mod, "HUD controller (DualSense: the player's car)", g_game + kRvaHudCtrl, reinterpret_cast<void*>(&detour_hud),
                                       reinterpret_cast<void**>(&g_orig_hud));
    const bool a = h1 && g_api->enable_hooks(g_mod, h1);
    const bool b = h2 && g_api->enable_hooks(g_mod, h2);
    say("car: the game's feedback %s (rumble %s, slips %s); Car update %s, HUD controller %s", ok_r && ok_h ? "hooked" : "NOT all hooked",
        ok_r ? "on" : "FAILED", ok_h ? "on" : "FAILED", a ? "on" : "FAILED", b ? "on" : "FAILED");
    return 0;
}

// ---- the controller --------------------------------------------------------------------------------------------------------------------
constexpr USHORT kVid = 0x054C, kPid = 0x0CE6, kPidEdge = 0x0DF2;
struct Ds {
    HANDLE h = nullptr;
    USHORT in_len = 0, out_len = 0, feat_len = 0;
    bool v2 = true, bt = false;
    BYTE seq = 0;
    OVERLAPPED wr{}, rd{};
    BYTE in[600];
};
Ds g_ds;
std::mutex g_write_mu;
std::atomic<bool> g_hap_running{false};   // an HD haptics stream is playing (USB audio or Bluetooth 0x32)
std::atomic<LONG> g_hap_gen{0};

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
                        if (caps.OutputReportByteLength < (bt ? 78 : 48) || caps.OutputReportByteLength > 600 || caps.InputReportByteLength > 600) {
                            std::snprintf(why, sizeof why, "the DualSense's reports are %u/%u bytes: not supported", caps.InputReportByteLength,
                                          caps.OutputReportByteLength);
                        } else if (bt && !S.bluetooth) {
                            std::snprintf(why, sizeof why, "the DualSense is on Bluetooth and Bluetooth effects are off (DualSense.ini Bluetooth=0)");
                        } else {
                            d.h        = h;
                            d.bt       = bt;
                            d.seq      = 0;
                            d.in_len   = caps.InputReportByteLength;
                            d.out_len  = caps.OutputReportByteLength;
                            d.feat_len = caps.FeatureReportByteLength;
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
    d.rd.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    d.v2        = true;
    if (d.feat_len && d.feat_len <= 600) {   // the firmware: rumble emulation v2 from update 0x0224 on
        BYTE feat[600] = {};
        feat[0]        = 0x20;
        if (HidD_GetFeature(d.h, feat, d.feat_len)) {
            const unsigned update = static_cast<unsigned>(feat[44]) | (static_cast<unsigned>(feat[45]) << 8);
            d.v2                  = update >= 0x0224;
            say("controller: DualSense over %s (firmware update %04X)", d.bt ? "Bluetooth" : "USB", update);
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
    std::scoped_lock lk{g_write_mu};
    CancelIoEx(d.h, nullptr);
    CloseHandle(d.h);
    if (d.wr.hEvent) CloseHandle(d.wr.hEvent);
    if (d.rd.hEvent) CloseHandle(d.rd.hEvent);
    d.wr = OVERLAPPED{};
    d.rd = OVERLAPPED{};
    d.h  = nullptr;
}

// One report as it is (its first byte the report id), padded to the output length the HID caps give.
bool ds_write_raw(Ds& d, const BYTE* rep, size_t n) {
    std::scoped_lock lk{g_write_mu};
    if (!d.h) return false;
    BYTE buf[600] = {};
    if (d.out_len > sizeof buf || n > d.out_len) return false;
    std::memcpy(buf, rep, n);
    ResetEvent(d.wr.hEvent);
    DWORD got = 0;
    if (!WriteFile(d.h, buf, d.out_len, nullptr, &d.wr)) {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(d.wr.hEvent, 200) != WAIT_OBJECT_0) {
            CancelIoEx(d.h, &d.wr);
            GetOverlappedResult(d.h, &d.wr, &got, TRUE);
            return false;
        }
    }
    return GetOverlappedResult(d.h, &d.wr, &got, FALSE) != 0;
}

// the common 47 bytes as a USB report 0x02 or a Bluetooth report 0x31 (a sequence, a tag, the bytes, a CRC seeded with 0xA2)
bool ds_write(Ds& d, const BYTE common[47]) {
    BYTE buf[78] = {};
    if (!d.bt) {
        buf[0] = 0x02;
        std::memcpy(buf + 1, common, 47);
        return ds_write_raw(d, buf, 48);
    }
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
    return ds_write_raw(d, buf, 78);
}

// ---- trigger effects (11 bytes; Nielk1's encoding) -----------------------------------------------------------------------------------
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
void trig_resistance(BYTE* e, int kind, int start, int f0, int f1) {
    BYTE force[10] = {};
    if (!kind) { trig_off(e); return; }
    for (int i = start; i < 10; ++i) {
        const float t = kind == 2 && start < 9 ? std::pow(static_cast<float>(i - start) / (9 - start), 1.7f) : 0;
        force[i] = static_cast<BYTE>(clampi(static_cast<int>(f0 + (f1 - f0) * t + 0.5f), 1, 8));
    }
    trig_zones(e, 0x21, force, 0);
}
void trig_vib(BYTE* e, int a, int hz) {
    BYTE amp[10];
    for (int i = 0; i < 10; ++i) amp[i] = static_cast<BYTE>(clampi(a, 1, 8));
    trig_zones(e, 0x26, amp, static_cast<BYTE>(clampi(hz, 1, 255)));
}
void trig_wall(BYTE* e, int pos, int force) {
    BYTE f[10] = {};
    for (int i = clampi(pos * 10 / 256, 0, 9); i < 10; ++i) f[i] = static_cast<BYTE>(force);
    trig_zones(e, 0x21, f, 0);
}
float slip_level(float slip, float from, float full) {
    const float t = clampf((slip - from) / (full > from + 0.5f ? full - from : 0.5f), 0, 1);
    return t * t * (3 - 2 * t);
}
float shift_knock(double now) {
    const double t = now - Car.shift_at;
    return Car.shift_at && t >= 0 && t < 0.15 ? std::exp(-static_cast<float>(t) / 0.05f) : 0;
}
bool latch(bool* state, double* since, float v, float off, float on, double hold, double now) {
    if (!*state && v >= on) { *state = true; *since = now; }
    else if (*state && v < off && now - *since >= hold) *state = false;
    return *state;
}
bool surf_trigger(BYTE* e, int cls, float level, float speed, int weaker) {
    int a, hz;
    switch (cls) {
        case SURF_KERB: a = 3; hz = static_cast<int>(clampf(speed / 0.95f, 8, 40)); break;
        case SURF_GRAVEL: a = 2; hz = 48; break;
        case SURF_DIRT: a = 2; hz = 26; break;
        case SURF_GRASS: a = 1; hz = 18; break;
        case SURF_SAND: a = 1; hz = 30; break;
        case SURF_SNOW: a = 1; hz = 14; break;
        case SURF_COBBLE: a = 2; hz = static_cast<int>(clampf(speed / 0.32f, 20, 60)); break;
        case SURF_METAL: a = 1; hz = 70; break;
        case SURF_BUMPY: a = 1; hz = 20; break;
        default: return false;
    }
    const float amp = a * surf_gain(cls, level, speed) * S.surface / 0.6f - weaker;
    if (amp < 0.6f) return false;
    trig_vib(e, static_cast<int>(amp + 0.5f), hz);
    return true;
}

constexpr float BRAKE_FULL = 3.0f, SPIN_FULL = 2.5f, LOCK_SLIP = 3.2f;
struct TrigState {
    bool lock_on = false, spin_on = false;
    double lock_since = 0, spin_since = 0;
} TS;

// e[0] L2 (the brake), e[1] R2 (the throttle)
void triggers_make(BYTE e[2][11], double now, bool front) {
    const int l2 = g_l2.load(), r2 = g_r2.load();
    bool set_l = false, set_r = false;
    if (!S.triggers || !front) { trig_off(e[0]); trig_off(e[1]); return; }
    if (!car_driving(now)) {
        if (S.free_in_menus) { trig_off(e[0]); trig_off(e[1]); }
        else {
            trig_resistance(e[0], S.brake_resist, S.brake_start, S.brake_f0, S.brake_f1);
            trig_resistance(e[1], S.throttle_resist, S.throttle_start, S.throttle_f, S.throttle_f);
        }
        TS.lock_on = TS.spin_on = false;
        return;
    }
    trig_resistance(e[0], S.brake_resist, S.brake_start, S.brake_f0, S.brake_f1);
    trig_resistance(e[1], S.throttle_resist, S.throttle_start, S.throttle_f, S.throttle_f);
    if (S.triggers != 2) return;
    const float hit = S.crash_jolt ? car_hit(now) : 0;
    if (hit > 0.12f) {
        const int a = 1 + static_cast<int>(hit * 6 + 0.5f);
        trig_vib(e[0], a, 20);
        trig_vib(e[1], a, 20);
        return;
    }
    if (Surf.land_at && now - Surf.land_at < 0.14 && S.events > 0) {
        const int a = 2 + static_cast<int>(Surf.land * 5 + 0.5f);
        trig_vib(e[0], a, 16);
        trig_vib(e[1], a, 16);
        return;
    }
    const float knock = S.gear_kick && (l2 > 24 || r2 > 24) ? shift_knock(now) : 0;
    const bool kick_l = knock > 0.3f && l2 > r2, kick_r = knock > 0.3f && !kick_l;
    // L2 past the front tyres' grip limit: with ABS its pump's strokes; without, the tyres scrub, and a locked wheel makes the
    // pedal go hard where it is, juddering slowly
    const float sb = Car.brake;
    if (kick_l) {
        trig_vib(e[0], 2 + static_cast<int>(knock * 3 + 0.5f), 14);
        set_l = true;
    } else if (Car.haptics_hooked && now < g_abs.act_until && l2 > 10) {
        const float k = slip_level(sb, 0.6f, BRAKE_FULL);
        set_l = true;
        trig_vib(e[0], (static_cast<int>(now * 28) & 1) ? 3 + static_cast<int>(k * 4 + 0.5f) : 1, static_cast<int>(S.brake_hz * (1 + 0.4f * k) + 0.5f));
    } else if (Car.haptics_hooked && latch(&TS.lock_on, &TS.lock_since, sb, S.brake_from - 0.1f, S.brake_from, 0.1, now) && l2 > 10) {
        const float k = slip_level(sb, S.brake_from, BRAKE_FULL);
        set_l = true;
        if (false)
            trig_vib(e[0], (static_cast<int>(now * 28) & 1) ? 3 + static_cast<int>(k * 4 + 0.5f) : 1, static_cast<int>(S.brake_hz * (1 + 0.4f * k) + 0.5f));
        else if (sb >= LOCK_SLIP) {
            if ((static_cast<int>(now * 9) & 3) == 0) trig_vib(e[0], 5, 10);
            else trig_wall(e[0], l2, 8);
        } else {
            trig_vib(e[0], 1 + static_cast<int>(k * 3 + 0.5f), static_cast<int>(S.brake_hz * 1.3f + 0.5f));
        }
    }
    // R2: wheel spin vibrates, harder and faster the more they spin; the rev limiter knocks
    const float sa = Car.spin;
    if (kick_r) {
        trig_vib(e[1], 2 + static_cast<int>(knock * 3 + 0.5f), 14);
        set_r = true;
    } else if (Car.haptics_hooked && latch(&TS.spin_on, &TS.spin_since, sa, S.spin_from - 0.1f, S.spin_from, 0.1, now) && r2 > 16) {
        const float k = slip_level(sa, S.spin_from, SPIN_FULL);
        trig_vib(e[1], 1 + static_cast<int>(k * 5 + 0.5f), static_cast<int>(S.throttle_hz * (1 + 0.4f * k) + 0.5f));
        set_r = true;
    } else if (S.rev_limiter && Car.lim && r2 > 60 && (static_cast<int>(now * 18) & 1)) {
        trig_vib(e[1], 4, 30);
        set_r = true;
    }
    // the road: its texture in the trigger pressed (the brake's a little lighter)
    if (Surf.works && now - Surf.at < 0.3 && S.surface > 0) {
        const int c    = surf_main();
        const float lv = surf_speed_level(Surf.speed);
        if (!set_r && r2 > 16) surf_trigger(e[1], c, lv, Surf.speed, 0);
        if (!set_l && l2 > 16) surf_trigger(e[0], c, lv, Surf.speed, 1);
    }
}

// ---- HD haptics: one synthesiser, two outputs (USB audio at the endpoint's rate, Bluetooth report 0x32 at 3 kHz) ---------------------
float rnd01(uint32_t* r) {
    *r = *r * 1664525u + 1013904223u;
    return static_cast<float>(*r >> 8) / 16777216.0f;
}
struct SurfOsc {
    int cls = SURF_NONE;
    float gain = 0;
    double ph = 0, ph2 = 0;
    float jit = 1, y1 = 0, y2 = 0;
    uint32_t rng = 0;
};
void surf_render(SurfOsc* o, int cls, float level, float gain, float speed, float* out, int n, double t0, double sr, uint32_t seed) {
    const double dt = 1.0 / sr;
    const float kg = 1 - std::exp(-1 / (0.02f * static_cast<float>(sr)));
    const float rhz = cls == SURF_GRAVEL ? 220.0f : cls == SURF_SAND ? 160.0f : 120.0f;
    const float rr = std::exp(-1 / ((cls == SURF_GRAVEL ? 0.006f : 0.009f) * static_cast<float>(sr))), rc = 2 * rr * std::cos(TAU * rhz / static_cast<float>(sr));
    const float sw = std::sin(TAU * rhz / static_cast<float>(sr));
    const float grit = cls == SURF_GRAVEL ? 150 + 550 * level : cls == SURF_SAND ? 30 + 90 * level : cls == SURF_SNOW ? 40 : 0;
    float target = cls > SURF_TARMAC ? gain : 0;
    if (cls != o->cls) {
        if (o->gain > 0.01f) { target = 0; cls = o->cls; }
        else { o->cls = cls; o->y1 = o->y2 = 0; o->ph = 0; o->jit = 1; }
    }
    if (!o->rng) o->rng = seed;
    for (int f = 0; f < n; ++f) {
        const double t = t0 + f * dt;
        float s = 0;
        o->gain += (target - o->gain) * kg;
        if (o->gain < 0.0005f && target == 0) continue;
        switch (cls) {
            case SURF_KERB: {
                const float rate = clampf(speed / 0.95f, 3, 40);
                o->ph += rate * dt;
                if (o->ph >= 1) o->ph -= std::floor(o->ph);
                s = 1.3f * fx_ping(static_cast<float>(o->ph / rate), 65, 0.016f);
                break;
            }
            case SURF_COBBLE: {
                const float rate = clampf(speed / 0.32f, 4, 45) * o->jit;
                o->ph += rate * dt;
                if (o->ph >= 1) { o->ph -= std::floor(o->ph); o->jit = 0.8f + 0.4f * rnd01(&o->rng); }
                s = 0.55f * fx_ping(static_cast<float>(o->ph / rate), 95, 0.009f);
                break;
            }
            case SURF_GRAVEL:
            case SURF_SAND:
            case SURF_SNOW: {
                const float x = rnd01(&o->rng) < grit * static_cast<float>(dt) ? (rnd01(&o->rng) * 2 - 1) * sw : 0;
                const float y = rc * o->y1 - rr * rr * o->y2 + x;
                o->y2 = o->y1;
                o->y1 = y;
                s = cls == SURF_GRAVEL ? 0.42f * y + 0.18f * fx_vnoise(t, 45, seed)
                    : cls == SURF_SAND ? 0.25f * y + 0.3f * fx_vnoise(t, 60, seed)
                                       : 0.2f * y + 0.25f * fx_vnoise(t, 22, seed);
                break;
            }
            case SURF_DIRT: s = 0.45f * fx_vnoise(t, 42, seed) + 0.25f * fx_vnoise(t, 9, seed + 1); break;
            case SURF_GRASS: s = 0.35f * fx_vnoise(t, 28, seed) + 0.15f * fx_vnoise(t, 6, seed + 1); break;
            case SURF_METAL: {
                const float hz = clampf(40 + 9 * speed, 60, 240);
                o->ph2 += hz * dt;
                if (o->ph2 >= 1) o->ph2 -= std::floor(o->ph2);
                s = 0.3f * std::sin(TAU * static_cast<float>(o->ph2)) * (0.7f + 0.3f * fx_vnoise(t, 15, seed));
                break;
            }
            case SURF_BUMPY: s = 0.4f * fx_vnoise(t, 16, seed) + 0.2f * fx_vnoise(t, 5, seed + 1); break;
            default: break;
        }
        out[f] += s * o->gain;
    }
}
float soft_limit(float x) {
    if (x > 3) return 1;
    if (x < -3) return -1;
    return x * (27 + x * x) / (27 + 9 * x * x);
}
void game_motors(float* lo, float* hi) {
    *lo = *hi = 0;
    if (Shared* sh = shared())
        if (static_cast<DWORD>(GetTickCount() - sh->tick) < 500) {
            *lo = clampf(sh->lm, 0, 1);
            *hi = clampf(sh->rm, 0, 1);
        }
}

struct Synth {
    double sr = 48000;
    double ph[3] = {0, 0, 0};
    double amp[2] = {0, 0}, eng = 0;
    SurfOsc road[2];
    // n frames from now + `ahead` seconds into left/right (-1..1)
    void render(float* left, float* right, int n, double ahead) {
        const double tau = 6.283185307179586;
        const double k = 1.0 - std::exp(-1.0 / (0.006 * sr));
        const double now = now_s(), t0 = now + ahead;
        const bool front = game_in_front();
        float glo = 0, ghi = 0;
        if (front && S.enabled) game_motors(&glo, &ghi);
        const float st = S.strength;
        float eng_t = 0;
        double eng_hz = 0;
        int road_cls[2] = {SURF_NONE, SURF_NONE};
        float level = 0, speed = 0;
        if (S.enabled && front && car_driving(now)) {
            if (S.car > 0) {
                const float rpm = clampf(Car.rpm, 0, 1.05f), thr = std::max(g_r2.load() / 255.0f, static_cast<float>(Car.throttle));
                eng_hz = 22 + 70 * rpm;
                eng_t  = S.car * 0.45f * (0.1f + 0.9f * rpm * rpm) * (0.4f + 0.6f * thr);
                if (S.rev_limiter && Car.lim) eng_t = (static_cast<int>(now * 16) & 1) ? S.car * 0.6f : S.car * 0.05f;
            }
            if (Surf.works && now - Surf.at < 0.3) {
                road_cls[0] = surf_side(0);
                road_cls[1] = surf_side(1);
                speed = Surf.speed;
                level = surf_speed_level(speed);
            }
        }
        std::fill(left, left + n, 0.0f);
        std::fill(right, right + n, 0.0f);
        if (S.enabled && front) fx_render(left, right, n, t0, 1.0 / sr);
        for (int i = 0; i < 2; ++i)
            surf_render(&road[i], road_cls[i], level, S.surface * surf_gain(road_cls[i], level, speed), speed, i ? right : left, n, t0, sr,
                        i ? 0xA7u : 0x51u);
        const double step0 = tau * S.low_hz / sr, step1 = tau * S.high_hz / sr;
        for (int f = 0; f < n; ++f) {
            amp[0] += (glo - amp[0]) * k;
            amp[1] += (ghi - amp[1]) * k;
            eng += (eng_t - eng) * k;
            const double lo = std::sin(ph[0]) * amp[0], hi = std::sin(ph[1]) * amp[1], car = std::sin(ph[2]) * eng;
            ph[0] += step0;
            ph[1] += step1;
            ph[2] += tau * eng_hz / sr;
            for (double& p : ph)
                if (p > tau) p -= tau;
            left[f]  = soft_limit(st * (static_cast<float>(lo + 0.35 * hi + car) + left[f]));
            right[f] = soft_limit(st * (static_cast<float>(hi + 0.35 * lo + car * 0.8) + right[f]));
        }
    }
};

// Bluetooth: report 0x32 every 32 stereo frames at 3 kHz (10.67 ms): a 0x11 packet (sized, 7 bytes: 0xFE 0 0 0 0 0xFF counter), a 0x12
// packet (sized, 64 bytes: 32 frames, 8-bit signed, left and right), a CRC32 over 0xA2 and the report at its end (142 bytes in all).
DWORD WINAPI bt_haptics_thread(LPVOID arg) {
    const LONG gen = static_cast<LONG>(reinterpret_cast<intptr_t>(arg));
    timeBeginPeriod(1);
    // set up: no motor power reduction, the haptics' power save and filter bits (as SAxense's users do)
    {
        BYTE c[47] = {};
        c[1]  = 0x02 | 0x20 | 0x40;
        c[9]  = 0;
        c[36] = 0;
        ds_write(g_ds, c);
    }
    g_hap_running = true;
    say("haptics: HD haptics over Bluetooth on (report 0x32, 3 kHz)");
    Synth syn;
    syn.sr = 3000;
    BYTE rep[142];
    BYTE counter = 0;
    float l[32], r[32];
    const double period = 32.0 / 3000.0;
    double next         = now_s();
    int fails           = 0;
    while (g_hap_gen.load() == gen && g_ds.h) {
        const double t = now_s();
        if (t < next) {
            const double wait = next - t;
            if (wait > 0.002) Sleep(static_cast<DWORD>((wait - 0.001) * 1000));
            continue;
        }
        if (t - next > 0.1) next = t;   // fell behind (a stall): no catching up
        next += period;
        syn.render(l, r, 32, 0.0);
        std::memset(rep, 0, sizeof rep);
        rep[0] = 0x32;
        rep[1] = 0x00;
        BYTE* p = rep + 2;
        *p++ = 0x80 | 0x11;
        *p++ = 7;
        const BYTE p11[7] = {0xFE, 0, 0, 0, 0, 0xFF, counter++};
        std::memcpy(p, p11, 7);
        p += 7;
        *p++ = 0x80 | 0x12;
        *p++ = 64;
        for (int i = 0; i < 32; ++i) {
            *p++ = static_cast<BYTE>(static_cast<int8_t>(clampi(static_cast<int>(std::lround(l[i] * 127.0f)), -127, 127)));
            *p++ = static_cast<BYTE>(static_cast<int8_t>(clampi(static_cast<int>(std::lround(r[i] * 127.0f)), -127, 127)));
        }
        const BYTE seed = 0xA2;
        const uint32_t crc = crc32(crc32(0, &seed, 1), rep, 138);
        rep[138] = static_cast<BYTE>(crc);
        rep[139] = static_cast<BYTE>(crc >> 8);
        rep[140] = static_cast<BYTE>(crc >> 16);
        rep[141] = static_cast<BYTE>(crc >> 24);
        if (!ds_write_raw(g_ds, rep, sizeof rep)) {
            if (++fails == 20) say("haptics: Bluetooth haptics reports aren't accepted (error %lu) -- rumble emulation instead", GetLastError());
            if (fails >= 20) break;
        } else {
            fails = 0;
        }
    }
    g_hap_running = false;
    timeEndPeriod(1);
    say("haptics: Bluetooth haptics stopped");
    return 0;
}

// USB: the controller's audio endpoint (4 channels: 3 and 4 drive the left and right actuators)
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const GUID kSubtypeFloat        = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

IMMDevice* find_endpoint(IMMDeviceEnumerator* en) {
    IMMDeviceCollection* col = nullptr;
    IMMDevice* dev           = nullptr;
    UINT count               = 0;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return nullptr;
    col->GetCount(&count);
    for (UINT i = 0; i < count && !dev; ++i) {
        IMMDevice* d       = nullptr;
        IPropertyStore* ps = nullptr;
        PROPVARIANT pv;
        if (FAILED(col->Item(i, &d))) continue;
        PropVariantInit(&pv);
        if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && SUCCEEDED(ps->GetValue(kFriendlyName, &pv)) && pv.vt == VT_LPWSTR &&
            (contains_ci(pv.pwszVal, L"DualSense") || contains_ci(pv.pwszVal, L"Wireless Controller"))) {
            dev = d;
            d   = nullptr;
        }
        PropVariantClear(&pv);
        if (ps) ps->Release();
        if (d) d->Release();
    }
    col->Release();
    return dev;
}

DWORD WINAPI usb_haptics_thread(LPVOID arg) {
    const LONG gen = static_cast<LONG>(reinterpret_cast<intptr_t>(arg));
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev          = nullptr;
    IAudioClient* ac        = nullptr;
    IAudioRenderClient* rc  = nullptr;
    WAVEFORMATEX* wf        = nullptr;
    HANDLE ev               = nullptr;
    UINT frames             = 0;
    float* side             = nullptr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en)))) goto done;
    for (int tries = 0; tries < 16 && g_hap_gen.load() == gen && !(dev = find_endpoint(en)); ++tries) Sleep(500);
    if (!dev) {
        if (g_hap_gen.load() == gen) say("haptics: no DualSense audio endpoint -- rumble emulation instead");
        goto done;
    }
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac))) || FAILED(ac->GetMixFormat(&wf))) goto done;
    if (wf->nChannels < 4) {
        say("haptics: the endpoint has %u channels, not 4 -- rumble emulation instead", wf->nChannels);
        goto done;
    }
    {
        const bool is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                              (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf)->SubFormat, kSubtypeFloat));
        const int bits = wf->wBitsPerSample;
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000, 0, wf, nullptr)) || FAILED(ac->SetEventHandle(ev)) ||
            FAILED(ac->GetBufferSize(&frames)) || FAILED(ac->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rc))) || FAILED(ac->Start())) {
            say("haptics: the audio stream didn't start -- rumble emulation instead");
            goto done;
        }
        say("haptics: HD haptics over USB on (%u channels, %lu Hz, %d-bit %s)", wf->nChannels, wf->nSamplesPerSec, bits, is_float ? "float" : "PCM");
        g_hap_running = true;
        Synth syn;
        syn.sr = wf->nSamplesPerSec;
        side   = static_cast<float*>(HeapAlloc(GetProcessHeap(), 0, sizeof(float) * 2 * (frames + 16)));
        while (side && g_hap_gen.load() == gen && g_ds.h) {
            UINT pad = 0;
            BYTE* buf;
            if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) continue;
            if (FAILED(ac->GetCurrentPadding(&pad))) break;
            const UINT n = frames - pad;
            if (!n || FAILED(rc->GetBuffer(n, &buf))) continue;
            float *left = side, *right = side + frames;
            syn.render(left, right, static_cast<int>(n), pad / syn.sr);
            for (UINT f = 0; f < n; ++f)
                for (int c = 0; c < wf->nChannels; ++c) {
                    const float v = c == 2 ? left[f] : c == 3 ? right[f] : 0.0f;
                    BYTE* p = buf + static_cast<size_t>(f) * wf->nBlockAlign + static_cast<size_t>(c) * (bits / 8);
                    if (is_float) *reinterpret_cast<float*>(p) = v;
                    else if (bits == 16) *reinterpret_cast<SHORT*>(p) = static_cast<SHORT>(v * 32767);
                    else if (bits == 24) { const int s = static_cast<int>(v * 8388607); p[0] = static_cast<BYTE>(s); p[1] = static_cast<BYTE>(s >> 8); p[2] = static_cast<BYTE>(s >> 16); }
                    else if (bits == 32) *reinterpret_cast<INT32*>(p) = static_cast<INT32>(v * 2147483647.0);
                }
            if (FAILED(rc->ReleaseBuffer(n, 0))) break;
        }
        ac->Stop();
        say("haptics: USB haptics stopped");
    }
done:
    g_hap_running = false;
    if (side) HeapFree(GetProcessHeap(), 0, side);
    if (rc) rc->Release();
    if (ac) ac->Release();
    if (wf) CoTaskMemFree(wf);
    if (dev) dev->Release();
    if (en) en->Release();
    if (ev) CloseHandle(ev);
    CoUninitialize();
    return 0;
}

void haptics_start() {
    const LONG gen = ++g_hap_gen;
    if (S.vibration != 2) return;
    if (HANDLE h = CreateThread(nullptr, 0, g_ds.bt ? bt_haptics_thread : usb_haptics_thread, reinterpret_cast<void*>(static_cast<intptr_t>(gen)), 0, nullptr))
        CloseHandle(h);
}
void haptics_stop() { ++g_hap_gen; }

// ---- the controller's input (for this mod: the triggers' positions, the menus' haptics, the PS button, the battery) --------------------
struct InState {
    WORD buttons = 0;
    bool ps_was = false;
    bool stick_out = false;
} IS;
constexpr WORD B_UP = 1, B_DOWN = 2, B_LEFT = 4, B_RIGHT = 8, B_A = 0x10, B_B = 0x20, B_LB = 0x40, B_RB = 0x80;

void menu_haptics(WORD btn, float sx, float sy, double now) {
    if (S.menus <= 0 || car_driving(now) || !game_in_front()) return;
    const WORD pressed = static_cast<WORD>(btn & ~IS.buttons);
    const float m = S.menus;
    if (pressed & (B_UP | B_DOWN | B_LEFT | B_RIGHT)) { fx_add(FX_TICK, 1.6f * m, 170, 0, 0.03f, 0.012f); fx_add(FX_THUMP, 0.9f * m, 80, 0, 0, 0.03f); }
    if (pressed & (B_LB | B_RB)) { fx_add(FX_TICK, 1.7f * m, 130, 0, 0.035f, 0.014f); fx_add(FX_THUMP, 1.0f * m, 70, 0, 0, 0.04f); }
    if (pressed & B_A) { fx_add(FX_THUMP, 1.8f * m, 75, 0, 0, 0.05f); fx_add(FX_TICK, 1.2f * m, 240, 0, 0.03f, 0.01f); }
    if (pressed & B_B) fx_add(FX_THUMP, 1.3f * m, 60, 0, 0, 0.05f);
    const bool out = std::fabs(sx) > 0.6f || std::fabs(sy) > 0.6f;   // the stick moving through a list
    if (out && !IS.stick_out) fx_add(FX_TICK, 1.3f * m, 170, 0, 0.03f, 0.012f);
    IS.stick_out = std::fabs(sx) > 0.4f || std::fabs(sy) > 0.4f ? out || IS.stick_out : false;
}

void parse_input(const Ds& d, const BYTE* rep, DWORD len) {
    const BYTE* r;
    BYTE simple[64] = {};
    bool full       = true;
    if (!d.bt && rep[0] == 0x01 && len >= 64) r = rep + 1;
    else if (d.bt && rep[0] == 0x31 && len >= 78) r = rep + 2;
    else if (d.bt && rep[0] == 0x01 && len >= 10) {
        simple[0] = rep[1]; simple[1] = rep[2]; simple[2] = rep[3]; simple[3] = rep[4];
        simple[4] = rep[8]; simple[5] = rep[9]; simple[7] = rep[5]; simple[8] = rep[6]; simple[9] = rep[7];
        r = simple;
        full = false;
    } else {
        return;
    }
    const double now = now_s();
    g_l2 = r[4];
    g_r2 = r[5];
    const BYTE b0 = r[7], b1 = r[8], b2 = r[9];
    WORD btn = 0;
    switch (b0 & 0x0F) {
        case 0: btn |= B_UP; break;
        case 2: btn |= B_RIGHT; break;
        case 4: btn |= B_DOWN; break;
        case 6: btn |= B_LEFT; break;
        default: break;
    }
    if (b0 & 0x20) btn |= B_A;
    if (b0 & 0x40) btn |= B_B;
    if (b1 & 0x01) btn |= B_LB;
    if (b1 & 0x02) btn |= B_RB;
    auto stick = [](BYTE c) { return c >= 128 ? (c - 128) / 127.0f : (c - 128) / 128.0f; };
    menu_haptics(btn, stick(r[0]), stick(r[1]), now);
    IS.buttons = btn;
    const bool ps = (b2 & 0x01) != 0;
    if (ps && !IS.ps_was && S.ps_battery) g_battery_show_until = now + 3.0;
    IS.ps_was = ps;
    if (full) {
        const BYTE st = r[52];
        const int cap = st & 0x0F, chg = st >> 4;
        g_charging = chg == 1;
        g_battery  = chg == 2 ? 100 : std::min(cap * 10 + 5, 100);
    }
}

// ---- the output report: rumble emulation, triggers, lights -------------------------------------------------------------------------
struct OutState {
    BYTE trig[2][11] = {};
    BYTE rgb[3] = {}, player = 0xFF, mic = 0xFF;
    BYTE report[47] = {};
    double full = 0, write = 0;
    bool first = true, emulated = false;
} OS;

BYTE mixb(int a, int b, float t) { return static_cast<BYTE>(a + (b - a) * clampf(t, 0, 1) + 0.5f); }

void surface_rumble(double now, float* lo, float* hi) {
    if (!Surf.works || now - Surf.at > 0.3 || S.surface <= 0) return;
    const float level = surf_speed_level(Surf.speed), g = S.surface, sp = Surf.speed;
    switch (surf_main()) {
        case SURF_KERB: if (std::fmod(now * clampf(sp / 0.95f, 3, 14), 1.0) < 0.5) *lo += g * 0.6f * surf_gain(SURF_KERB, level, sp); break;
        case SURF_COBBLE: if (std::fmod(now * clampf(sp / 0.32f, 4, 14), 1.0) < 0.4) *hi += g * 0.3f * level; break;
        case SURF_GRAVEL: *hi += g * level * (0.18f + 0.1f * std::fabs(fx_vnoise(now, 30, 7))); break;
        case SURF_SAND: *hi += g * level * 0.1f; *lo += g * level * 0.08f; break;
        case SURF_SNOW: *lo += g * level * 0.08f; break;
        case SURF_DIRT: *lo += g * level * (0.12f + 0.1f * std::fabs(fx_vnoise(now, 8, 3))); break;
        case SURF_GRASS: *lo += g * level * 0.1f; break;
        case SURF_METAL: *hi += g * level * 0.14f; break;
        case SURF_BUMPY: *lo += g * level * 0.12f * std::fabs(fx_vnoise(now, 6, 5)); break;
        default: break;
    }
}

void send_output(Ds& d, bool force) {
    BYTE c[47] = {};
    const double now  = now_s();
    const bool full   = force || OS.first || now - OS.full > 1.0;
    const bool front  = game_in_front();
    const bool driving = car_driving(now) && front;
    const float st    = S.strength;

    // rumble: the motors' emulation, unless an HD haptics stream carries it
    float lo = 0, hi = 0;
    if (front && S.enabled) game_motors(&lo, &hi);
    lo *= st;
    hi *= st;
    const bool emulate = S.enabled && S.vibration >= 1 && !g_hap_running.load();
    if (emulate) {
        float flo = 0, fhi = 0;
        if (driving && S.car > 0) {
            const float thr = std::max(g_r2.load() / 255.0f, static_cast<float>(Car.throttle));
            flo = S.car * 0.25f * (0.1f + 0.9f * Car.rpm * Car.rpm) * (0.4f + 0.6f * thr);
            if (S.rev_limiter && Car.lim && (static_cast<int>(now * 16) & 1)) fhi = std::max(fhi, S.car * 0.4f);
        }
        if (driving) surface_rumble(now, &flo, &fhi);
        fx_rumble(now, &flo, &fhi);
        lo = std::max(lo, flo * st);
        hi = std::max(hi, fhi * st);
    } else {
        fx_rumble(now, &lo, &hi);   // keeps the voices' list tidy; the stream plays them
        lo = hi = 0;
    }
    if (emulate || OS.emulated) {   // the emulation, or its last values cleared once the HD haptics take over
        c[0] |= 0x02;
        if (d.v2) c[38] |= 0x04;
        else c[0] |= 0x01;
        if (emulate) {
            c[2] = static_cast<BYTE>(clampf(hi, 0, 1) * 255.0f);
            c[3] = static_cast<BYTE>(clampf(lo, 0, 1) * 255.0f);
        }
        OS.emulated = emulate;
    }

    // triggers
    BYTE e[2][11];
    if (S.enabled) triggers_make(e, now, front);
    else { trig_off(e[0]); trig_off(e[1]); }
    if (full || std::memcmp(e, OS.trig, sizeof e)) {
        c[0] |= 0x04 | 0x08;
        std::memcpy(c + 10, e[1], 11);
        std::memcpy(c + 21, e[0], 11);
        std::memcpy(OS.trig, e, sizeof e);
        c[36] = static_cast<BYTE>(clampi(static_cast<int>((1.0f - S.trig_strength) * 8), 0, 7));
        c[1] |= 0x40;
    }

    // the light bar
    BYTE rgb[3] = {0, 0, 0};
    const int battery = g_battery.load();
    switch (S.light_mode) {
        case 1: for (int i = 0; i < 3; ++i) rgb[i] = static_cast<BYTE>(S.color[i]); break;
        case 2: {
            float t = std::max(car_hit(now) * 1.2f, std::max(lo, hi));
            for (int i = 0; i < 3; ++i) rgb[i] = mixb(S.color[i], S.flash[i], t);
            break;
        }
        case 3: {
            const float b = battery / 100.0f;
            rgb[0] = static_cast<BYTE>(255 * (1 - b));
            rgb[1] = static_cast<BYTE>(255 * b);
            break;
        }
        case 4:
            for (int i = 0; i < 3; ++i) rgb[i] = static_cast<BYTE>(S.color[i]);
            if (driving) {
                static const int red[3] = {255, 0, 0};
                const float t = clampf((Car.rpm - 0.55f) / 0.4f, 0, 1);
                for (int i = 0; i < 3; ++i) rgb[i] = mixb(S.color[i], red[i], t);
                if (Car.lim && (static_cast<int>(now * 12) & 1)) rgb[0] = rgb[1] = rgb[2] = 0;
            }
            break;
        default: break;
    }
    for (int i = 0; i < 3; ++i) rgb[i] = static_cast<BYTE>(rgb[i] * S.brightness);
    if (S.low_battery_blink && battery && battery <= 15 && !g_charging.load() && (static_cast<int>(now * 2) & 1)) { rgb[0] = 255; rgb[1] = rgb[2] = 0; }
    if (OS.first) {
        c[38] |= 0x02;   // light bar setup: ends the blue start-up animation
        c[41] = 0x02;
    }
    if (S.light_mode && (full || std::memcmp(rgb, OS.rgb, 3))) {
        c[1] |= 0x04;
        c[44] = rgb[0]; c[45] = rgb[1]; c[46] = rgb[2];
        std::memcpy(OS.rgb, rgb, 3);
    }

    // player lights: player 1, the battery after the PS button, or a rev counter (from 70% of the redline, flashing on the limiter)
    static const BYTE bars[6] = {0x00, 0x01, 0x03, 0x07, 0x0F, 0x1F};
    BYTE player = 0x04;
    if (now < g_battery_show_until.load()) player = bars[clampi((battery + 19) / 20, 0, 5)];
    else if (S.rev_lights && driving) {
        if (Car.lim) player = (static_cast<int>(now * 10) & 1) ? 0x1F : 0x00;
        else player = Car.rpm < 0.70f ? 0x00 : bars[clampi(static_cast<int>((Car.rpm - 0.70f) / 0.055f) + 1, 1, 5)];
    }
    if (full || player != OS.player) {
        c[1] |= 0x10;
        c[43]     = player;
        OS.player = player;
    }
    // the mic light: gyro steering (the platform's)
    const BYTE mic = static_cast<BYTE>(shared() && shared()->gyro_on ? 1 : 0);
    if (full || mic != OS.mic) {
        c[1] |= 0x01;
        c[8]   = mic;
        OS.mic = mic;
    }
    if (!full && !OS.first && !std::memcmp(c, OS.report, sizeof c) && now - OS.write < 0.25) return;
    if (ds_write(d, c)) {
        OS.first = false;
        std::memcpy(OS.report, c, sizeof c);
        OS.write = now;
        if (full) OS.full = now;
    } else {
        say("controller: gone (write failed, error %lu, report %u bytes)", GetLastError(), d.out_len);
        haptics_stop();
        ds_close(d);
    }
}

// the controller back to itself: no rumble, no trigger effects, its own lights
void send_reset(Ds& d) {
    if (!d.h) return;
    BYTE c[47] = {};
    c[0] = 0x02 | 0x01 | 0x04 | 0x08;
    trig_off(c + 10);
    trig_off(c + 21);
    c[1] = 0x08 | 0x01;
    ds_write(d, c);
}

DWORD WINAPI controller_thread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    double next_scan = 0;
    bool reading     = false;
    bool was_neutral = false;
    for (;;) {
        Ds& d = g_ds;
        if (!d.h) {
            reading = false;
            if (now_s() < next_scan || !S.enabled) { Sleep(250); continue; }
            next_scan = now_s() + 2.0;
            if (!ds_open(d)) continue;
            OS = OutState{};
            fx_clear();
            haptics_start();
            send_output(d, true);
            was_neutral = false;
        }
        if (!reading) {
            ResetEvent(d.rd.hEvent);
            if (!ReadFile(d.h, d.in, d.in_len, nullptr, &d.rd) && GetLastError() != ERROR_IO_PENDING) {
                say("controller: gone (read error %lu)", GetLastError());
                haptics_stop();
                ds_close(d);
                continue;
            }
            reading = true;
        }
        const double now = now_s();
        const double tick = car_driving(now) ? 0.012 : 0.03;
        if (WaitForSingleObject(d.rd.hEvent, static_cast<DWORD>(tick * 1000)) == WAIT_OBJECT_0) {
            DWORD got = 0;
            reading   = false;
            if (!GetOverlappedResult(d.h, &d.rd, &got, FALSE)) {
                say("controller: gone (read error %lu)", GetLastError());
                haptics_stop();
                ds_close(d);
                continue;
            }
            parse_input(d, d.in, got);
        }
        const bool neutral = !S.enabled || !game_in_front();
        if (neutral) {
            if (!was_neutral) {   // switched off, or another window in front: the controller is itself again
                send_reset(d);
                OS = OutState{};
            }
            was_neutral = true;
            continue;
        }
        was_neutral = false;
        if (now - OS.write >= tick * 0.8 || OS.first) send_output(d, false);
    }
}

// ---- the Mods page ---------------------------------------------------------------------------------------------------------------------
void on_toggle(void*, const char* key, int value) {
    struct { const char* k; const wchar_t* ini; int* v; } rows[] = {
        {"Enabled", L"Enabled", &S.enabled}, {"Bluetooth", L"Bluetooth", &S.bluetooth}, {"RevLights", L"RevLights", &S.rev_lights},
        {"Vibration", L"Vibration", &S.vibration}, {"Triggers", L"Triggers", &S.triggers}, {"LightBar", L"LightBar", &S.light_mode}};
    for (auto& r : rows)
        if (!std::strcmp(key, r.k)) {
            *r.v = value;
            save(r.ini, static_cast<float>(value));
            if (!std::strcmp(key, "Vibration") && g_ds.h) { haptics_stop(); Sleep(30); haptics_start(); }
        }
}
void on_slide(void*, const char* key, float value) {
    struct { const char* k; const wchar_t* ini; float* v; } rows[] = {
        {"Strength", L"Strength", &S.strength}, {"EngineFeel", L"EngineFeel", &S.car}, {"Collisions", L"Collisions", &S.collisions},
        {"RoadSurface", L"RoadSurface", &S.surface}, {"Events", L"Events", &S.events}, {"MenuHaptics", L"MenuHaptics", &S.menus},
        {"TriggerStrength", L"TriggerStrength", &S.trig_strength}};
    for (auto& r : rows)
        if (!std::strcmp(key, r.k)) { *r.v = value; save(r.ini, value); }
}

void add_rows(const FH3ModApi* api) {
    auto toggle = [&](const char* key, const char* label, const char* desc, int on) {
        FH3Setting s{};
        s.size = sizeof s; s.key = key; s.label = label; s.description = desc; s.value = on ? 1 : 0; s.on_change = on_toggle;
        api->add_setting(g_mod, &s);
    };
    auto slider = [&](const char* key, const char* label, const char* desc, float v, float mx) {
        FH3Setting s{};
        s.size = sizeof s; s.key = key; s.label = label; s.description = desc; s.kind = FH3_SETTING_SLIDER; s.min = 0; s.max = mx; s.step = 0.05f;
        s.slider_value = v; s.display_scale = 100; s.decimals = 0; s.on_slide = on_slide;
        api->add_setting(g_mod, &s);
    };
    auto choice = [&](const char* key, const char* label, const char* desc, int v, const char* const* names, uint32_t n) {
        FH3Setting s{};
        s.size = sizeof s; s.key = key; s.label = label; s.description = desc; s.kind = FH3_SETTING_CHOICE; s.value = v; s.choices = names;
        s.choice_count = n; s.on_change = on_toggle;
        api->add_setting(g_mod, &s);
    };
    static const char* const vib[]   = {"Off", "Rumble", "HD haptics"};
    static const char* const trig[]  = {"Off", "Resistance", "Full"};
    static const char* const light[] = {"Off", "Colour", "Impacts", "Battery", "Revs"};
    toggle("Enabled", "DualSense effects", "The DualSense's vibration, triggers and lights follow the car. Off gives the controller back to itself.", S.enabled);
    if (api->api_version >= 9) choice("Vibration", "Vibration", "HD haptics: the controller's actuators (USB audio, or Bluetooth reports). Rumble: its motors' emulation.", S.vibration, vib, 3);
    slider("Strength", "Vibration strength (%)", "Everything felt in the controller.", S.strength, 2.0f);
    slider("EngineFeel", "Engine feel (%)", "The engine as a hum at its revs, knocking on the limiter, gear changes.", S.car, 1.0f);
    slider("Collisions", "Collisions (%)", "Crashes, by how hard they are.", S.collisions, 1.0f);
    slider("RoadSurface", "Road surface (%)", "Gravel, dirt, grass, sand, snow, cobbles, kerbs under each side of the car, and in the trigger you press.", S.surface, 1.0f);
    slider("Events", "Jumps and landings (%)", "Take-offs and landings.", S.events, 1.0f);
    slider("MenuHaptics", "Menu haptics (%)", "Ticks and clicks as you move through the menus.", S.menus, 1.0f);
    if (api->api_version >= 9) choice("Triggers", "Adaptive triggers", "Resistance: the pedals. Full: also ABS, lock-ups, wheel spin, the rev limiter, gear changes, crashes and the road.", S.triggers, trig, 3);
    slider("TriggerStrength", "Trigger strength (%)", "How hard the trigger effects push.", S.trig_strength, 1.0f);
    if (api->api_version >= 9) choice("LightBar", "Light bar", "Revs: to red at the redline. Impacts: flashes on crashes and rumble. Battery: green to red.", S.light_mode, light, 5);
    toggle("RevLights", "Rev counter on the player lights", "The five player lights count the revs; the PS button shows the battery.", S.rev_lights);
    toggle("Bluetooth", "Bluetooth", "Also over Bluetooth: the controller is switched to its full reports (the platform reads them). Takes effect when it reconnects.", S.bluetooth);
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
    info.description      = "DualSense (USB and Bluetooth): HD haptics of the engine, the road, crashes and the menus; adaptive triggers with ABS, lock-ups and wheel spin; rev-counter lights.";
    info.offline_only     = 1;
    g_mod                 = api->register_mod(&info);
    if (!g_mod) return 1;
    g_cfg_path = std::wstring(api->azimuth_dir) + L"\\fh3-mods\\DualSense.ini";
    load_config();
    crc_init();
    add_rows(api);
    if (HANDLE t = CreateThread(nullptr, 0, install_hooks, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, controller_thread, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}
