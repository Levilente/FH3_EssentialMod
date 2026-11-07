// Every controller for the mods that read XInput (platform 1.1.0).
//
// The mods (Nitro, Airride, Launch Control, ...) and the platform's own hotkeys read the pads with XInputGetState,
// which only lists Xbox-like controllers: a DualSense, a DualShock 4 or any other HID gamepad that isn't presented
// to Windows as an Xbox 360 pad (ViGEm, DS4Windows, Steam Input) is invisible to them, even when the game itself
// is given the pad some other way. This file closes that gap without touching a single mod:
//
//  - A reader thread lists the PC's other game controllers with Windows.Gaming.Input.RawGameController (the same
//    list Windows keeps for every HID gamepad: USB or Bluetooth, no driver), reads them ~250 times a second and
//    maps what it finds onto an Xbox pad: Sony pads by their known layout, everything else by the usual HID order
//    (A B X Y LB RB Back Start LS RS, left stick on the first two axes). Which of the remaining axes are the right
//    stick and which the triggers is learned from the resting values, so it doesn't depend on the order Windows lists
//    them in. Xbox controllers (vendor 045E) are left to XInput.
//  - XInputGetState is hooked (MinHook) in xinput1_4.dll. For the *mods* (any DLL in fh3-mods, and the platform's
//    own code) that pad is added to the answer: OR-ed into a real XInput pad's buttons, or a pad in slot 0 when XInput
//    has none. Every other caller (the game, Azimuth, Extra Options, other mods' loaders) gets XInput's answer
//    untouched, so the game never sees a second controller.
//
// Nothing here writes to the game or to a mod; with `[input] Enabled=0` in fh3-mods\platform.ini it all stays off,
// `[input] Raw=0` keeps the hook but drops the controller reader, and `[input] Ignore=054C:0CE6,...` skips devices.
#include "platform.hpp"
#include "updates.hpp"

#include <windows.h>

#include "MinHook.h"

#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fh3platform {

namespace {

struct PadState { // XINPUT_STATE
    DWORD packet;
    WORD buttons;
    BYTE lt, rt;
    SHORT lx, ly, rx, ry;
};

constexpr WORD X_UP = 0x0001, X_DOWN = 0x0002, X_LEFT = 0x0004, X_RIGHT = 0x0008, X_START = 0x0010, X_BACK = 0x0020,
               X_LS = 0x0040, X_RS = 0x0080, X_LB = 0x0100, X_RB = 0x0200, X_A = 0x1000, X_B = 0x2000, X_X = 0x4000,
               X_Y = 0x8000;

// ---- the merged pad the reader thread keeps -------------------------------------------------------------------------
struct Virt {
    bool present = false;
    DWORD packet = 0;
    WORD buttons = 0;
    BYTE lt = 0, rt = 0;
    SHORT lx = 0, ly = 0, rx = 0, ry = 0;
};
std::mutex g_vmu;
Virt g_virt;
std::atomic<bool> g_present{false};     // the RawGameController pads
std::atomic<bool> g_hid_present{false}; // a DualSense read directly (USB or Bluetooth)
std::mutex g_hmu;
Virt g_hid;

// ---- WinRT by hand (no WinRT headers in MinGW) ----------------------------------------------------------------------
using RoInitializeFn            = HRESULT(WINAPI*)(int);
using RoGetActivationFactoryFn  = HRESULT(WINAPI*)(void*, REFIID, void**);
using WindowsCreateStringFn     = HRESULT(WINAPI*)(const wchar_t*, UINT32, void**);
using WindowsDeleteStringFn     = HRESULT(WINAPI*)(void*);

const IID kIidRawStatics = {0xEB8D0792, 0xE95A, 0x4B19, {0xAF, 0xC7, 0x0A, 0x59, 0xF8, 0xBF, 0x75, 0x9E}};
const IID kIidRawController = {0x7CAD6D91, 0xA7E1, 0x4F71, {0x9A, 0x78, 0x33, 0xE9, 0xC5, 0xDF, 0xEA, 0x62}};

template <class F> F vslot(void* obj, int i) { return reinterpret_cast<F>((*reinterpret_cast<void***>(obj))[i]); }
ULONG com_release(void* o) { return o ? vslot<ULONG(WINAPI*)(void*)>(o, 2)(o) : 0; }

struct Dev {
    void* obj = nullptr; // IRawGameController
    uint16_t vid = 0, pid = 0;
    int buttons = 0, switches = 0, axes = 0;
    bool sony = false;
    bool skip = false; // listed, not read (an Xbox pad, or on the Ignore list)
    // learned from the resting axes
    bool locked = false;
    int stable = 0;
    int stick[2] = {-1, -1};
    int trig[2]  = {-1, -1};
    std::vector<uint8_t> b;
    std::vector<int32_t> sw;
    std::vector<double> a;
};

std::vector<Dev> g_devs;
std::vector<uint32_t> g_ignore; // vid << 16 | pid

std::string hex4(unsigned v) {
    char s[8];
    std::snprintf(s, sizeof s, "%04X", v & 0xFFFF);
    return s;
}

std::wstring ini_path() { return (azimuth_dir() / "fh3-mods" / "platform.ini").wstring(); }

void read_ignore() {
    wchar_t buf[512] = L"";
    GetPrivateProfileStringW(L"input", L"Ignore", L"", buf, 512, ini_path().c_str());
    std::wstring s = buf;
    size_t i       = 0;
    while (i < s.size()) {
        size_t j = s.find(L',', i);
        if (j == std::wstring::npos) j = s.size();
        const std::wstring item = s.substr(i, j - i);
        const size_t colon      = item.find(L':');
        if (colon != std::wstring::npos) {
            const unsigned v = static_cast<unsigned>(std::wcstoul(item.substr(0, colon).c_str(), nullptr, 16));
            const unsigned p = static_cast<unsigned>(std::wcstoul(item.substr(colon + 1).c_str(), nullptr, 16));
            g_ignore.push_back((v << 16) | (p & 0xFFFF));
        }
        i = j + 1;
    }
}

bool ignored(uint16_t vid, uint16_t pid) {
    const uint32_t k = (static_cast<uint32_t>(vid) << 16) | pid;
    return std::find(g_ignore.begin(), g_ignore.end(), k) != g_ignore.end();
}

// ---- mapping a device onto an Xbox pad ------------------------------------------------------------------------------
// HID button order of Sony's pads: Square Cross Circle Triangle L1 R1 L2 R2 Create Options L3 R3 PS Touchpad.
const WORD kSonyButtons[12]    = {X_X, X_A, X_B, X_Y, X_LB, X_RB, 0, 0, X_BACK, X_START, X_LS, X_RS};
// The usual order of any other gamepad's HID buttons.
const WORD kGenericButtons[10] = {X_A, X_B, X_X, X_Y, X_LB, X_RB, X_BACK, X_START, X_LS, X_RS};

void classify_axes(Dev& d) {
    if (d.locked) return;
    const int extra = d.axes - 2;
    if (extra < 2) {
        d.locked = true;
        return;
    }
    if (extra < 4) { // right stick on the next two, no analog triggers
        d.stick[0] = 2;
        d.stick[1] = 3;
        d.locked   = true;
        return;
    }
    int mid[4], low[4], nm = 0, nl = 0;
    for (int k = 0; k < 4; ++k) {
        const double v = d.a[2 + k];
        if (v >= 0.35 && v <= 0.65) mid[nm++] = 2 + k;
        else if (v <= 0.04) low[nl++] = 2 + k;
        else {
            nm = nl = 9; // not a resting picture
            break;
        }
    }
    if (nm == 2 && nl == 2) {
        if (++d.stable >= 30) {
            d.stick[0] = mid[0];
            d.stick[1] = mid[1];
            d.trig[0]  = low[0];
            d.trig[1]  = low[1];
            d.locked   = true;
            plog("input: " + hex4(d.vid) + ":" + hex4(d.pid) + " axes learned: right stick on " + std::to_string(d.stick[0]) + "/" +
                 std::to_string(d.stick[1]) + ", triggers on " + std::to_string(d.trig[0]) + "/" + std::to_string(d.trig[1]));
        }
    } else {
        d.stable = 0;
    }
}

SHORT stick_short(double v, bool invert) {
    double s = (v - 0.5) * 2.0;
    if (invert) s = -s;
    s = std::max(-1.0, std::min(1.0, s));
    return static_cast<SHORT>(std::lround(s * 32767.0));
}

BYTE trig_byte(double v) {
    v = std::max(0.0, std::min(1.0, v));
    return static_cast<BYTE>(std::lround(v * 255.0));
}

void add_to(Virt& v, Dev& d) {
    WORD btn = 0;
    const int nb = std::min<int>(d.buttons, static_cast<int>(d.b.size()));
    if (d.sony) {
        for (int i = 0; i < nb && i < 12; ++i)
            if (d.b[i]) btn |= kSonyButtons[i];
    } else {
        for (int i = 0; i < nb && i < 10; ++i)
            if (d.b[i]) btn |= kGenericButtons[i];
    }
    if (d.switches > 0 && !d.sw.empty()) {
        switch (d.sw[0]) {
            case 1: btn |= X_UP; break;
            case 2: btn |= X_UP | X_RIGHT; break;
            case 3: btn |= X_RIGHT; break;
            case 4: btn |= X_DOWN | X_RIGHT; break;
            case 5: btn |= X_DOWN; break;
            case 6: btn |= X_DOWN | X_LEFT; break;
            case 7: btn |= X_LEFT; break;
            case 8: btn |= X_UP | X_LEFT; break;
            default: break;
        }
    }
    classify_axes(d);
    SHORT lx = 0, ly = 0, rx = 0, ry = 0;
    BYTE lt = 0, rt = 0;
    if (d.axes >= 2) {
        lx = stick_short(d.a[0], false);
        ly = stick_short(d.a[1], true);
    }
    if (d.stick[0] >= 0) rx = stick_short(d.a[d.stick[0]], false);
    if (d.stick[1] >= 0) ry = stick_short(d.a[d.stick[1]], true);
    if (d.trig[0] >= 0) {
        lt = trig_byte(d.a[d.trig[0]]);
        rt = trig_byte(d.a[d.trig[1]]);
    } else if (d.sony && nb > 7) { // no analog triggers learned (yet): the digital L2/R2
        if (d.b[6]) lt = 255;
        if (d.b[7]) rt = 255;
    }
    v.buttons |= btn;
    v.lt = std::max(v.lt, lt);
    v.rt = std::max(v.rt, rt);
    auto bigger = [](SHORT cur, SHORT nw) { return std::abs(static_cast<int>(nw)) > std::abs(static_cast<int>(cur)) ? nw : cur; };
    v.lx = bigger(v.lx, lx);
    v.ly = bigger(v.ly, ly);
    v.rx = bigger(v.rx, rx);
    v.ry = bigger(v.ry, ry);
    v.present = true;
}

// ---- the reader thread ---------------------------------------------------------------------------------------------
void rescan(void* statics) {
    void* view = nullptr;
    if (FAILED(vslot<HRESULT(WINAPI*)(void*, void**)>(statics, 10)(statics, &view)) || !view) return;
    UINT32 n = 0;
    vslot<HRESULT(WINAPI*)(void*, UINT32*)>(view, 7)(view, &n);
    std::vector<Dev> now;
    for (UINT32 i = 0; i < n && i < 16; ++i) {
        void* raw = nullptr;
        if (FAILED(vslot<HRESULT(WINAPI*)(void*, UINT32, void**)>(view, 6)(view, i, &raw)) || !raw) continue;
        void* ctl = nullptr;
        const HRESULT qi = vslot<HRESULT(WINAPI*)(void*, REFIID, void**)>(raw, 0)(raw, kIidRawController, &ctl);
        com_release(raw);
        if (FAILED(qi) || !ctl) continue;
        bool known = false;
        for (Dev& o : g_devs)
            if (o.obj == ctl) {
                now.push_back(std::move(o));
                o.obj = nullptr;
                com_release(ctl); // the one we already hold stays
                known = true;
                break;
            }
        if (known) continue;
        Dev d;
        d.obj = ctl;
        int32_t ax = 0, bt = 0, sw = 0;
        uint16_t vid = 0, pid = 0;
        vslot<HRESULT(WINAPI*)(void*, int32_t*)>(ctl, 6)(ctl, &ax);
        vslot<HRESULT(WINAPI*)(void*, int32_t*)>(ctl, 7)(ctl, &bt);
        vslot<HRESULT(WINAPI*)(void*, uint16_t*)>(ctl, 9)(ctl, &pid);
        vslot<HRESULT(WINAPI*)(void*, uint16_t*)>(ctl, 10)(ctl, &vid);
        vslot<HRESULT(WINAPI*)(void*, int32_t*)>(ctl, 11)(ctl, &sw);
        d.vid = vid;
        d.pid = pid;
        if (ax < 0 || ax > 64 || bt < 0 || bt > 128 || sw < 0 || sw > 8) { // not a plausible answer: leave it alone
            com_release(ctl);
            continue;
        }
        d.axes     = ax;
        d.buttons  = bt;
        d.switches = sw;
        d.sony     = vid == 0x054C;
        d.b.assign(static_cast<size_t>(std::max(bt, 1)), 0);
        d.sw.assign(static_cast<size_t>(std::max(sw, 1)), 0);
        d.a.assign(static_cast<size_t>(std::max(ax, 1)), 0.5);
        if (vid == 0x045E) { // Xbox pads (and the X360 pads other tools make) are XInput's own
            plog("input: controller " + hex4(vid) + ":" + hex4(pid) + " is an Xbox pad: XInput reads it");
            d.skip = true;
            now.push_back(std::move(d));
            continue;
        }
        if (ignored(vid, pid)) {
            plog("input: controller " + hex4(vid) + ":" + hex4(pid) + " is on the Ignore list");
            d.skip = true;
            now.push_back(std::move(d));
            continue;
        }
        if (ax + bt + sw == 0) {
            d.skip = true;
            now.push_back(std::move(d));
            continue;
        }
        plog("input: controller found " + hex4(vid) + ":" + hex4(pid) + " with " + std::to_string(bt) + " buttons, " + std::to_string(ax) +
             " axes, " + std::to_string(sw) + " d-pad(s), read as " + (d.sony ? "a Sony pad" : "a generic gamepad"));
        now.push_back(std::move(d));
    }
    com_release(view);
    for (Dev& o : g_devs) {
        if (o.obj) {
            plog("input: controller " + hex4(o.vid) + ":" + hex4(o.pid) + " is gone");
            com_release(o.obj);
        }
    }
    g_devs = std::move(now);
}

bool safe_rescan(void* statics) {
    __try {
        rescan(statics);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool safe_read(Dev& d) {
    UINT64 ts = 0;
    HRESULT hr = E_FAIL;
    __try {
        hr = vslot<HRESULT(WINAPI*)(void*, UINT32, uint8_t*, UINT32, int32_t*, UINT32, double*, UINT64*)>(d.obj, 13)(
            d.obj, static_cast<UINT32>(d.b.size()), d.b.data(), static_cast<UINT32>(d.sw.size()), d.sw.data(),
            static_cast<UINT32>(d.a.size()), d.a.data(), &ts);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        hr = E_FAIL;
    }
    return SUCCEEDED(hr);
}

DWORD WINAPI reader(LPVOID) {
    HMODULE combase = LoadLibraryW(L"combase.dll");
    if (!combase) return 0;
    auto ro_init    = reinterpret_cast<RoInitializeFn>(reinterpret_cast<void*>(GetProcAddress(combase, "RoInitialize")));
    auto ro_factory = reinterpret_cast<RoGetActivationFactoryFn>(reinterpret_cast<void*>(GetProcAddress(combase, "RoGetActivationFactory")));
    auto str_new    = reinterpret_cast<WindowsCreateStringFn>(reinterpret_cast<void*>(GetProcAddress(combase, "WindowsCreateString")));
    auto str_free   = reinterpret_cast<WindowsDeleteStringFn>(reinterpret_cast<void*>(GetProcAddress(combase, "WindowsDeleteString")));
    if (!ro_init || !ro_factory || !str_new || !str_free) {
        plog("input: Windows.Gaming.Input isn't available: only XInput pads reach the mods");
        return 0;
    }
    ro_init(1); // RO_INIT_MULTITHREADED; "already initialised" is fine
    static const wchar_t cls[] = L"Windows.Gaming.Input.RawGameController";
    void* hs                   = nullptr;
    void* statics              = nullptr;
    if (FAILED(str_new(cls, static_cast<UINT32>(std::wcslen(cls)), &hs)) || !hs ||
        FAILED(ro_factory(hs, kIidRawStatics, &statics)) || !statics) {
        if (hs) str_free(hs);
        plog("input: the game controller list isn't available: only XInput pads reach the mods");
        return 0;
    }
    str_free(hs);
    ULONGLONG next_scan = 0;
    for (;;) {
        Sleep(4);
        const ULONGLONG t = GetTickCount64();
        if (t >= next_scan) {
            next_scan = t + 1000;
            if (!safe_rescan(statics)) {
                plog("input: a fault while listing the controllers; the list is dropped");
                g_devs.clear();
            }
        }
        Virt v;
        for (Dev& d : g_devs) {
            if (!d.obj || d.skip) continue;
            // a DualSense the direct reader has: its state there is the real one (over Bluetooth the Windows list goes stale)
            if (d.sony && (d.pid == 0x0CE6 || d.pid == 0x0DF2) && g_hid_present.load(std::memory_order_relaxed)) continue;
            if (safe_read(d)) add_to(v, d);
        }
        std::scoped_lock lk{g_vmu};
        v.packet = g_virt.packet + 1;
        g_virt   = v;
        g_present.store(v.present, std::memory_order_relaxed);
    }
}


// ---- shared with mods (the DualSense mod): "Local\FH3ModPlatform.Vibration" ----------------------------------------------------
// The game's rumble as the engine sends it to the controller the platform gave it, when the car's feedback last came in (a mod
// that hooks it writes driving_tick: the mouse moves the menus' cursor only when it knows the car isn't being driven), and
// whether gyro steering is on (the DualSense mod lights the mic button for it).
struct Shared {
    float lm, rm, lt, rt;
    DWORD tick;
    volatile DWORD driving_tick;
    volatile DWORD gyro_on;
    volatile DWORD xbox_tick; // an XInput pad was last used (the DualSense mod writes ds_tick after it: the later one is in use)
    volatile DWORD ds_tick;
    DWORD reserved[7];
};
Shared* g_shared = nullptr;

// ---- the controls' settings: fh3-mods\DualSense.ini [Controller], [Keyboard], [Keys], [Mouse] (as FH2-Input's) -------------------
std::wstring ds_ini() { return (azimuth_dir() / "fh3-mods" / "DualSense.ini").wstring(); }
std::wstring ini_str(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[256] = L"";
    const std::wstring f = ds_ini();
    GetPrivateProfileStringW(sec, key, L"\x01", buf, 256, f.c_str());
    if (buf[0] == 1 && !buf[1]) { // not there: written with its default, so the file lists every setting
        WritePrivateProfileStringW(sec, key, def, f.c_str());
        return def;
    }
    return buf;
}
float ini_float(const wchar_t* sec, const wchar_t* key, float def) {
    wchar_t d[32];
    _snwprintf(d, 32, L"%g", static_cast<double>(def));
    const std::wstring v = ini_str(sec, key, d);
    return v.empty() ? def : static_cast<float>(std::wcstod(v.c_str(), nullptr));
}
int ini_int(const wchar_t* sec, const wchar_t* key, int def) { return static_cast<int>(std::lround(ini_float(sec, key, static_cast<float>(def)))); }
bool ieq(const std::wstring& a, const wchar_t* b) { return CompareStringOrdinal(a.c_str(), -1, b, -1, TRUE) == CSTR_EQUAL; }

WORD button_of(const std::wstring& n) {
    static const struct { const wchar_t* name; WORD bit; } names[] = {
        {L"A", X_A}, {L"B", X_B}, {L"X", X_X}, {L"Y", X_Y}, {L"LB", X_LB}, {L"RB", X_RB}, {L"LS", X_LS}, {L"RS", X_RS}, {L"Menu", X_START},
        {L"View", X_BACK}, {L"DPadUp", X_UP}, {L"DPadDown", X_DOWN}, {L"DPadLeft", X_LEFT}, {L"DPadRight", X_RIGHT}};
    for (const auto& b : names)
        if (ieq(n, b.name)) return b.bit;
    return 0;
}

struct ControllerSettings {
    bool swap = false;           // Circle confirms, Cross goes back
    WORD touch = X_BACK, create = X_BACK;
    bool swipes = true;
    WORD lpaddle = X_X, rpaddle = X_B, lfn = 0, rfn = 0;
    int gyro = 1;                // 0 off, 1 the mic button switches it, 2 always
    float gyro_max = 45.0f;      // degrees of tilt for a full lock
    bool gyro_invert = false;
} S;

void load_controller_settings() {
    const wchar_t* c = L"Controller";
    S.swap        = ini_int(c, L"SwapCrossCircle", 0) != 0;
    S.touch       = button_of(ini_str(c, L"TouchpadClick", L"View"));
    S.create      = button_of(ini_str(c, L"CreateButton", L"View"));
    S.swipes      = ini_int(c, L"TouchpadSwipes", 1) != 0;
    S.lpaddle     = button_of(ini_str(c, L"EdgeLeftPaddle", L"X"));
    S.rpaddle     = button_of(ini_str(c, L"EdgeRightPaddle", L"B"));
    S.lfn         = button_of(ini_str(c, L"EdgeLeftFn", L""));
    S.rfn         = button_of(ini_str(c, L"EdgeRightFn", L""));
    const std::wstring g = ini_str(c, L"Gyro", L"MicButton");
    S.gyro        = ieq(g, L"Off") || ieq(g, L"0") ? 0 : ieq(g, L"Always") || ieq(g, L"2") ? 2 : 1;
    S.gyro_max    = std::max(5.0f, ini_float(c, L"GyroMaxAngle", 45.0f));
    S.gyro_invert = ini_int(c, L"GyroInvert", 0) != 0;
}

// The DualSense's own state between reports: the touchpad's swipe, gyro steering, the buttons that toggle.
struct DsState {
    bool touching = false, swiped = false;
    int x0 = 0, y0 = 0;
    ULONGLONG pulse_until = 0;
    WORD pulse = 0;
    bool gyro_on = false, mic_was = false, have_g = false;
    float g[3] = {0, 0, 0};
    float neutral = 0;
} T;

// ---- the keyboard as a gamepad, and the mouse on it (FH2-Input's keyboard.h and mouse.h) ---------------------------------------
enum { KB_THROTTLE, KB_BRAKE, KB_LEFT, KB_RIGHT, KB_STICK_UP, KB_STICK_DOWN, KB_LOOK_LEFT, KB_LOOK_RIGHT, KB_LOOK_UP, KB_LOOK_DOWN,
       KB_A, KB_B, KB_X, KB_Y, KB_LB, KB_RB, KB_LS, KB_RS, KB_MENU, KB_VIEW, KB_DUP, KB_DDOWN, KB_DLEFT, KB_DRIGHT, KB_COUNT };
const struct { const wchar_t* key; const wchar_t* def; WORD button; } kKb[KB_COUNT] = {
    {L"Throttle", L"W", 0}, {L"Brake", L"S", 0}, {L"SteerLeft", L"A", 0}, {L"SteerRight", L"D", 0}, {L"StickUp", L"", 0},
    {L"StickDown", L"", 0}, {L"LookLeft", L"J", 0}, {L"LookRight", L"L", 0}, {L"LookUp", L"I", 0}, {L"LookDown", L"K", 0},
    {L"A", L"Space, Enter", X_A}, {L"B", L"E, Backspace", X_B}, {L"X", L"Q", X_X}, {L"Y", L"R", X_Y}, {L"LB", L"LShift", X_LB},
    {L"RB", L"C", X_RB}, {L"LeftStickClick", L"F", X_LS}, {L"RightStickClick", L"V", X_RS}, {L"Menu", L"Escape", X_START},
    {L"View", L"Tab", X_BACK}, {L"DPadUp", L"Up", X_UP}, {L"DPadDown", L"Down", X_DOWN}, {L"DPadLeft", L"Left", X_LEFT},
    {L"DPadRight", L"Right", X_RIGHT}};
constexpr int KB_MAX_KEYS = 4;
BYTE g_kb_keys[KB_COUNT][KB_MAX_KEYS];
bool g_kb_on = true, g_kb_focus = true;
float g_kb_steer_speed = 3, g_kb_steer_return = 5, g_kb_pedal = 8, g_kb_look = 0;
std::mutex g_kmu;
float g_kb_steer = 0, g_kb_thr = 0, g_kb_brake = 0, g_kb_lx = 0, g_kb_ly = 0;
double g_kb_last = 0;
std::atomic<bool> g_kb_joined{false};

BYTE vk_of(std::wstring n) {
    static const struct { const wchar_t* name; BYTE vk; } names[] = {
        {L"Space", VK_SPACE}, {L"Enter", VK_RETURN}, {L"Return", VK_RETURN}, {L"Backspace", VK_BACK}, {L"Tab", VK_TAB}, {L"Escape", VK_ESCAPE},
        {L"Esc", VK_ESCAPE}, {L"Shift", VK_SHIFT}, {L"LShift", VK_LSHIFT}, {L"RShift", VK_RSHIFT}, {L"Ctrl", VK_CONTROL}, {L"LCtrl", VK_LCONTROL},
        {L"RCtrl", VK_RCONTROL}, {L"Alt", VK_MENU}, {L"LAlt", VK_LMENU}, {L"RAlt", VK_RMENU}, {L"Up", VK_UP}, {L"Down", VK_DOWN}, {L"Left", VK_LEFT},
        {L"Right", VK_RIGHT}, {L"Home", VK_HOME}, {L"End", VK_END}, {L"PageUp", VK_PRIOR}, {L"PageDown", VK_NEXT}, {L"Insert", VK_INSERT},
        {L"Delete", VK_DELETE}, {L"CapsLock", VK_CAPITAL}, {L"NumpadPlus", VK_ADD}, {L"NumpadMinus", VK_SUBTRACT}, {L"NumpadStar", VK_MULTIPLY},
        {L"NumpadSlash", VK_DIVIDE}, {L"NumpadDot", VK_DECIMAL}, {L"Comma", VK_OEM_COMMA}, {L"Period", VK_OEM_PERIOD}, {L"Minus", VK_OEM_MINUS},
        {L"Equals", VK_OEM_PLUS}, {L"Semicolon", VK_OEM_1}, {L"Slash", VK_OEM_2}, {L"Backquote", VK_OEM_3}, {L"LBracket", VK_OEM_4},
        {L"Backslash", VK_OEM_5}, {L"RBracket", VK_OEM_6}, {L"Quote", VK_OEM_7}, {L"Mouse1", VK_LBUTTON}, {L"Mouse2", VK_RBUTTON},
        {L"Mouse3", VK_MBUTTON}, {L"Mouse4", VK_XBUTTON1}, {L"Mouse5", VK_XBUTTON2}};
    if (n.size() == 1) {
        wchar_t c = static_cast<wchar_t>(std::towupper(n[0]));
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return static_cast<BYTE>(c);
    }
    if ((n[0] == L'F' || n[0] == L'f') && n.size() >= 2 && n.size() <= 3 && std::iswdigit(n[1])) {
        const int f = std::wcstol(n.c_str() + 1, nullptr, 10);
        if (f >= 1 && f <= 24) return static_cast<BYTE>(VK_F1 + f - 1);
    }
    if (n.size() == 7 && CompareStringOrdinal(n.c_str(), 6, L"Numpad", 6, TRUE) == CSTR_EQUAL && std::iswdigit(n[6]))
        return static_cast<BYTE>(VK_NUMPAD0 + (n[6] - L'0'));
    for (const auto& k : names)
        if (ieq(n, k.name)) return k.vk;
    return 0;
}

// the mouse
enum { MS_HOLD, MS_SPEED };
enum { MS_MENU_CURSOR, MS_MENU_LOOK, MS_MENU_OFF };
bool g_ms_on = true, g_ms_invert = false, g_ms_lock = true, g_ms_clicks = true;
int g_ms_style = MS_HOLD, g_ms_menu = MS_MENU_CURSOR;
float g_ms_sens = 1, g_ms_return = 3, g_ms_delay = 0.3f, g_ms_menu_sens = 1;
WORD g_ms_wheel_up = X_UP, g_ms_wheel_down = X_DOWN;
std::atomic<LONG> g_ms_dx{0}, g_ms_dy{0}, g_ms_wheel{0};
float g_ms_x = 0, g_ms_y = 0;
bool g_ms_in_menu = false;
int g_ms_notches = 0, g_ms_part = 0;
double g_ms_moved_at = 0, g_ms_taken_at = 0, g_ms_press_until = 0;
WORD g_ms_press = 0;
bool g_ms_clipped = false;

void load_kb_settings() {
    g_kb_on           = ini_int(L"Keyboard", L"Enabled", 1) != 0;
    g_kb_focus        = ini_int(L"Keyboard", L"OnlyWhenGameInFront", 1) != 0;
    g_kb_steer_speed  = ini_float(L"Keyboard", L"SteerSpeed", 3.0f);
    g_kb_steer_return = ini_float(L"Keyboard", L"SteerReturn", 5.0f);
    g_kb_pedal        = ini_float(L"Keyboard", L"PedalSpeed", 8.0f);
    g_kb_look         = ini_float(L"Keyboard", L"LookSpeed", 0.0f);
    for (int c = 0; c < KB_COUNT; ++c) {
        std::memset(g_kb_keys[c], 0, KB_MAX_KEYS);
        const std::wstring list = ini_str(L"Keys", kKb[c].key, kKb[c].def);
        int k = 0;
        size_t i = 0;
        while (i <= list.size()) {
            size_t j = list.find(L',', i);
            if (j == std::wstring::npos) j = list.size();
            std::wstring name = list.substr(i, j - i);
            while (!name.empty() && name.front() == L' ') name.erase(name.begin());
            while (!name.empty() && name.back() == L' ') name.pop_back();
            if (!name.empty()) {
                const BYTE vk = vk_of(name);
                if (vk && k < KB_MAX_KEYS) g_kb_keys[c][k++] = vk;
                else if (!vk) plog("input: unknown key name in DualSense.ini [Keys]: " + narrow(name));
            }
            i = j + 1;
        }
    }
    const wchar_t* m = L"Mouse";
    g_ms_on        = g_kb_on && ini_int(m, L"Enabled", 1) != 0;
    g_ms_style     = ieq(ini_str(m, L"Look", L"Hold"), L"Speed") ? MS_SPEED : MS_HOLD;
    g_ms_sens      = ini_float(m, L"Sensitivity", 1.0f);
    g_ms_return    = ini_float(m, L"Return", 3.0f);
    g_ms_delay     = ini_float(m, L"ReturnAfter", 0.3f);
    g_ms_invert    = ini_int(m, L"InvertY", 0) != 0;
    g_ms_lock      = ini_int(m, L"KeepCursorInWindow", 1) != 0;
    const std::wstring mm = ini_str(m, L"Menus", L"Cursor");
    g_ms_menu      = ieq(mm, L"Look") ? MS_MENU_LOOK : ieq(mm, L"Off") ? MS_MENU_OFF : MS_MENU_CURSOR;
    g_ms_menu_sens = ini_float(m, L"MenuSensitivity", 1.0f);
    const std::wstring w = ini_str(m, L"Wheel", L"DPad");
    if (ieq(w, L"Shoulders")) { g_ms_wheel_up = X_LB; g_ms_wheel_down = X_RB; }
    else if (ieq(w, L"Off")) g_ms_wheel_up = g_ms_wheel_down = 0;
    else { g_ms_wheel_up = X_UP; g_ms_wheel_down = X_DOWN; }
    g_ms_clicks = ini_int(m, L"Clicks", 1) != 0;
    plog(std::string("input: keyboard ") + (g_kb_on ? "on (it joins the game's gamepads at the first key press)" : "off") + ", mouse " +
         (g_ms_on ? "on" : "off"));
}

double now_s() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}
bool game_front() {
    HWND w    = GetForegroundWindow();
    DWORD pid = 0;
    return w && GetWindowThreadProcessId(w, &pid) && pid == GetCurrentProcessId();
}
bool kb_down(int c) {
    for (int k = 0; k < KB_MAX_KEYS && g_kb_keys[c][k]; ++k)
        if (GetAsyncKeyState(g_kb_keys[c][k]) & 0x8000) return true;
    return false;
}
bool kb_active() { return g_kb_on && (!g_kb_focus || game_front()); }
bool kb_any() {
    if (!kb_active()) return false;
    for (int c = 0; c < KB_COUNT; ++c)
        if (kb_down(c)) return true;
    return false;
}
float approach(float v, float target, float rate, float dt) {
    const float d = target - v, step = rate * dt;
    if (rate <= 0 || (d <= step && d >= -step)) return target;
    return v + (d > 0 ? step : -step);
}
float clamp1(float v) { return v > 1 ? 1 : v < -1 ? -1 : v; }

// the menus: the car's feedback (a mod's driving_tick) isn't coming; unknown without such a mod (then the mouse looks around)
bool in_menus() {
    if (!g_shared || !g_shared->driving_tick) return false;
    return GetTickCount() - g_shared->driving_tick > 350;
}

void ms_clip() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    RECT r;
    POINT p = {0, 0};
    if (w) GetWindowThreadProcessId(w, &pid);
    if (g_ms_lock && g_kb_joined.load() && g_ms_moved_at > 0 && w && pid == GetCurrentProcessId() && !IsIconic(w) && GetClientRect(w, &r) &&
        r.right > r.left && r.bottom > r.top && ClientToScreen(w, &p)) {
        OffsetRect(&r, p.x, p.y);
        ClipCursor(&r);
        g_ms_clipped = true;
    } else if (g_ms_clipped) {
        ClipCursor(nullptr);
        g_ms_clipped = false;
    }
}
LRESULT CALLBACK ms_wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_INPUT) {
        RAWINPUT ri;
        UINT n = sizeof ri;
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &n, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
            ri.header.dwType == RIM_TYPEMOUSE) {
            if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && (ri.data.mouse.lLastX || ri.data.mouse.lLastY)) {
                g_ms_dx += ri.data.mouse.lLastX;
                g_ms_dy += ri.data.mouse.lLastY;
            }
            if (ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) g_ms_wheel += static_cast<SHORT>(ri.data.mouse.usButtonData);
        }
    } else if (m == WM_TIMER) {
        ms_clip();
    }
    return DefWindowProcW(w, m, wp, lp);
}
DWORD WINAPI ms_thread(LPVOID) {
    WNDCLASSW wc{};
    wc.lpfnWndProc   = ms_wndproc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FH3ModPlatformMouse";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage     = 0x02;
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = w;
    if (!w || !RegisterRawInputDevices(&rid, 1, sizeof rid)) {
        plog("input: no raw mouse input (" + std::to_string(GetLastError()) + "): the mouse doesn't look around");
        return 0;
    }
    SetTimer(w, 1, 250, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}
WORD ms_wheel_buttons(bool menu, double now) {
    const int w = static_cast<int>(g_ms_wheel.exchange(0));
    if (!menu || !g_ms_wheel_up) { g_ms_part = g_ms_notches = 0; g_ms_press = 0; return 0; }
    g_ms_part += w;
    g_ms_notches += g_ms_part / 120;
    g_ms_part %= 120;
    g_ms_notches = std::max(-5, std::min(5, g_ms_notches));
    if (now < g_ms_press_until) return g_ms_press;
    if (now < g_ms_press_until + 0.05 || !g_ms_notches) return 0;
    g_ms_press = g_ms_notches > 0 ? g_ms_wheel_up : g_ms_wheel_down;
    g_ms_notches += g_ms_notches > 0 ? -1 : 1;
    g_ms_press_until = now + 0.07;
    return g_ms_press;
}
void ms_poll(bool in, double now, float dt, float st[4], WORD* buttons) {
    st[0] = st[1] = st[2] = st[3] = 0;
    if (!g_ms_on || !in) {
        g_ms_dx = 0; g_ms_dy = 0; g_ms_wheel = 0;
        g_ms_x = g_ms_y = 0;
        return;
    }
    const bool menu = g_ms_menu != MS_MENU_LOOK && in_menus();
    *buttons |= ms_wheel_buttons(menu, now);
    if (menu && g_ms_clicks)
        *buttons |= static_cast<WORD>(((GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? X_A : 0) | ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) ? X_B : 0));
    if (menu != g_ms_in_menu) { g_ms_x = g_ms_y = 0; g_ms_in_menu = menu; }
    if (menu && g_ms_menu == MS_MENU_OFF) { g_ms_dx = 0; g_ms_dy = 0; return; }
    const bool speed = menu || g_ms_style == MS_SPEED;
    if (speed) {
        dt = static_cast<float>(now - g_ms_taken_at);
        if (dt < 0.004f) { st[menu ? 0 : 2] = g_ms_x; st[menu ? 1 : 3] = g_ms_y; return; }
        if (dt > 0.1f) dt = 0.1f;
        g_ms_taken_at = now;
    }
    const float dx = static_cast<float>(g_ms_dx.exchange(0));
    float dy       = static_cast<float>(g_ms_dy.exchange(0));
    if (g_ms_invert && !menu) dy = -dy;
    if (dx || dy) g_ms_moved_at = now;
    if (!speed) {
        g_ms_x = clamp1(g_ms_x + dx * g_ms_sens / 500.0f);
        g_ms_y = clamp1(g_ms_y - dy * g_ms_sens / 500.0f);
        if (now - g_ms_moved_at >= g_ms_delay) {
            g_ms_x = approach(g_ms_x, 0, g_ms_return, dt);
            g_ms_y = approach(g_ms_y, 0, g_ms_return, dt);
        }
    } else {
        const float k = dt * 20.0f > 1 ? 1 : dt * 20.0f, sens = (menu ? g_ms_menu_sens : g_ms_sens) / 2000.0f;
        g_ms_x += (clamp1(dx / dt * sens) - g_ms_x) * k;
        g_ms_y += (clamp1(-dy / dt * sens) - g_ms_y) * k;
        if (g_ms_x < 0.01f && g_ms_x > -0.01f && !dx) g_ms_x = 0;
        if (g_ms_y < 0.01f && g_ms_y > -0.01f && !dy) g_ms_y = 0;
    }
    st[menu ? 0 : 2] = g_ms_x;
    st[menu ? 1 : 3] = g_ms_y;
}
SHORT axis_s(float v) { v = clamp1(v); return static_cast<SHORT>(v >= 0 ? v * 32767.0f : v * 32768.0f); }

// the keyboard's pad now (with the mouse on it)
Virt kb_poll() {
    Virt g;
    if (!g_kb_on) return g;
    if (!g_kb_joined.load() && kb_any()) {
        g_kb_joined = true;
        plog("input: the keyboard is in use: its gamepad joins the game's");
    }
    if (!g_kb_joined.load()) return g;
    std::scoped_lock lk{g_kmu};
    const double now = now_s();
    float dt         = g_kb_last ? static_cast<float>(now - g_kb_last) : 0;
    const bool in    = kb_active();
    g_kb_last        = now;
    if (dt > 0.1f) dt = 0.1f;
    const float steer = in ? static_cast<float>(kb_down(KB_RIGHT) - kb_down(KB_LEFT)) : 0;
    g_kb_steer = approach(g_kb_steer, steer, steer == 0 || steer * g_kb_steer < 0 ? g_kb_steer_return : g_kb_steer_speed, dt);
    g_kb_thr   = approach(g_kb_thr, in && kb_down(KB_THROTTLE) ? 1.0f : 0.0f, g_kb_pedal, dt);
    g_kb_brake = approach(g_kb_brake, in && kb_down(KB_BRAKE) ? 1.0f : 0.0f, g_kb_pedal, dt);
    g_kb_lx    = approach(g_kb_lx, in ? static_cast<float>(kb_down(KB_LOOK_RIGHT) - kb_down(KB_LOOK_LEFT)) : 0, g_kb_look, dt);
    g_kb_ly    = approach(g_kb_ly, in ? static_cast<float>(kb_down(KB_LOOK_UP) - kb_down(KB_LOOK_DOWN)) : 0, g_kb_look, dt);
    float ms[4];
    WORD btn = 0;
    ms_poll(in, now, dt, ms, &btn);
    g.lx = axis_s(g_kb_steer + ms[0]);
    g.ly = axis_s((in ? static_cast<float>(kb_down(KB_STICK_UP) - kb_down(KB_STICK_DOWN)) : 0) + ms[1]);
    g.rx = axis_s(g_kb_lx + ms[2]);
    g.ry = axis_s(g_kb_ly + ms[3]);
    g.rt = static_cast<BYTE>(g_kb_thr * 255.0f + 0.5f);
    g.lt = static_cast<BYTE>(g_kb_brake * 255.0f + 0.5f);
    if (in)
        for (int c = 0; c < KB_COUNT; ++c)
            if (kKb[c].button && kb_down(c)) btn |= kKb[c].button;
    g.buttons = btn;
    g.present = true;
    return g;
}

// ---- a DualSense read straight from its HID device (USB and Bluetooth) -------------------------------------------------------
// Over Bluetooth the controller sends the short report 0x01 until something asks for the full one (a program lighting it up,
// FH2-Input, Steam); from then on it sends 0x31 only, and Windows' own gamepad list shows nothing of it. Reading the device
// ourselves (a handle of its own: the game and every other program keep theirs) sees both, and USB too.
bool is_ds(USHORT vid, USHORT pid) { return vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2); }

HANDLE hid_open(USHORT* in_len, bool* bt, bool* edge) {
    GUID guid;
    HidD_GetHidGuid(&guid);
    HDEVINFO set = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize    = sizeof ifd;
    HANDLE result = INVALID_HANDLE_VALUE;
    for (DWORD i = 0; result == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &guid, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        std::vector<char> mem(need + 16);
        auto* det   = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(mem.data());
        det->cbSize = sizeof *det;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) continue;
        std::wstring path = det->DevicePath;
        std::transform(path.begin(), path.end(), path.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (path.find(L"054c") == std::wstring::npos) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES a{};
        a.Size = sizeof a;
        PHIDP_PREPARSED_DATA pp = nullptr;
        bool ok                 = false;
        if (HidD_GetAttributes(h, &a) && is_ds(a.VendorID, a.ProductID) && HidD_GetPreparsedData(h, &pp)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 1 && caps.Usage == 5 && caps.InputReportByteLength >= 10 &&
                caps.InputReportByteLength <= 200) {
                *in_len = caps.InputReportByteLength;
                *bt     = caps.InputReportByteLength != 64;
                *edge   = a.ProductID == 0x0DF2;
                result  = h;
                ok      = true;
            }
            HidD_FreePreparsedData(pp);
        }
        if (!ok) CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return result;
}

void hid_parse(const BYTE* rep, DWORD n, bool bt, bool edge, Virt& v) {
    // the same fields in three layouts: USB 0x01 and Bluetooth 0x31 (the full report: X Y RX RY L2 R2 seq buttons0..3, then the
    // gyro, the accelerometer, the touchpad, the battery), Bluetooth 0x01 (the short one: sticks, buttons, triggers)
    const BYTE* r;
    BYTE simple[64] = {};
    bool full = true;
    if (rep[0] == 0x31 && bt && n >= 78) r = rep + 2;
    else if (rep[0] == 0x01 && !bt && n >= 64) r = rep + 1;
    else if (rep[0] == 0x01 && bt && n >= 10) {
        simple[0] = rep[1]; simple[1] = rep[2]; simple[2] = rep[3]; simple[3] = rep[4];
        simple[4] = rep[8]; simple[5] = rep[9];
        simple[7] = rep[5]; simple[8] = rep[6]; simple[9] = rep[7];
        r    = simple;
        full = false;
    } else {
        return;
    }
    const BYTE b0 = r[7], b1 = r[8], b2 = r[9];
    const WORD cross = S.swap ? X_B : X_A, circle = S.swap ? X_A : X_B;
    WORD btn = 0;
    if (b0 & 0x10) btn |= X_X;
    if (b0 & 0x20) btn |= cross;
    if (b0 & 0x40) btn |= circle;
    if (b0 & 0x80) btn |= X_Y;
    switch (b0 & 0x0F) {
        case 0: btn |= X_UP; break;
        case 1: btn |= X_UP | X_RIGHT; break;
        case 2: btn |= X_RIGHT; break;
        case 3: btn |= X_DOWN | X_RIGHT; break;
        case 4: btn |= X_DOWN; break;
        case 5: btn |= X_DOWN | X_LEFT; break;
        case 6: btn |= X_LEFT; break;
        case 7: btn |= X_UP | X_LEFT; break;
        default: break;
    }
    if (b1 & 0x01) btn |= X_LB;
    if (b1 & 0x02) btn |= X_RB;
    if (b1 & 0x10) btn |= S.create;
    if (b1 & 0x20) btn |= X_START;
    if (b1 & 0x40) btn |= X_LS;
    if (b1 & 0x80) btn |= X_RS;
    if (b2 & 0x02) btn |= S.touch;
    if (edge) {
        if (b2 & 0x10) btn |= S.lfn;
        if (b2 & 0x20) btn |= S.rfn;
        if (b2 & 0x40) btn |= S.lpaddle;
        if (b2 & 0x80) btn |= S.rpaddle;
    }
    // 0..255 around 128 onto -1..1
    auto stick = [](BYTE c) { return c >= 128 ? (c - 128) / 127.0f : (c - 128) / 128.0f; };
    float lx = stick(r[0]);
    // the mic button: gyro steering on/off
    const bool mic = (b2 & 0x04) != 0;
    if (mic && !T.mic_was && S.gyro == 1) {
        T.gyro_on = !T.gyro_on;
        T.have_g  = false;
        plog(std::string("input: gyro steering ") + (T.gyro_on ? "on" : "off"));
    }
    T.mic_was = mic;
    if (g_shared) g_shared->gyro_on = (S.gyro == 2 || (S.gyro == 1 && T.gyro_on)) ? 1 : 0;
    const ULONGLONG now = GetTickCount64();
    if (full) {
        // the touchpad, finger 1: a swipe is a D-pad press in its direction
        const BYTE* pt = r + 32;
        const bool touch = !(pt[0] & 0x80);
        const int x = pt[1] | (pt[2] & 0x0F) << 8, y = pt[2] >> 4 | pt[3] << 4;
        if (touch && !T.touching) {
            T.x0 = x; T.y0 = y; T.swiped = false;
        } else if (touch && !T.swiped && S.swipes) {
            const int dx = x - T.x0, dy = y - T.y0;
            if (dx * dx > 350 * 350 || dy * dy > 220 * 220) {
                T.swiped = true;
                T.pulse  = std::abs(dx) * 1080 > std::abs(dy) * 1920 ? (dx > 0 ? X_RIGHT : X_LEFT) : (dy > 0 ? X_DOWN : X_UP);
                T.pulse_until = now + 120;
            }
        }
        T.touching = touch;
        // gyro steering: the tilt (roll) from the accelerometer, filtered, from where it was when switched on
        if (S.gyro == 2 || (S.gyro == 1 && T.gyro_on)) {
            float a[3];
            for (int i = 0; i < 3; ++i) a[i] = static_cast<float>(static_cast<SHORT>(r[21 + 2 * i] | r[22 + 2 * i] << 8));
            if (!T.have_g) for (int i = 0; i < 3; ++i) T.g[i] = a[i];
            else for (int i = 0; i < 3; ++i) T.g[i] += (a[i] - T.g[i]) * 0.15f;
            const float m = std::sqrt(T.g[0] * T.g[0] + T.g[1] * T.g[1] + T.g[2] * T.g[2]);
            const float roll = m > 1000 ? std::asin(std::max(-1.0f, std::min(1.0f, T.g[0] / m))) * 57.29578f : 0;
            if (!T.have_g) {
                T.neutral = roll;
                T.have_g  = true;
            }
            float steer = -(roll - T.neutral) / S.gyro_max;
            if (S.gyro_invert) steer = -steer;
            lx += steer;
        }
    }
    if (now < T.pulse_until) btn |= T.pulse;
    v.buttons = btn;
    v.lt      = r[4];
    v.rt      = r[5];
    v.lx      = axis_s(lx);
    v.ly      = axis_s(-stick(r[1]));
    v.rx      = axis_s(stick(r[2]));
    v.ry      = axis_s(-stick(r[3]));
    v.present = true;
}

DWORD WINAPI hid_reader(LPVOID) {
    for (;;) {
        USHORT in_len = 0;
        bool bt = false, edge = false;
        HANDLE h      = hid_open(&in_len, &bt, &edge);
        if (h == INVALID_HANDLE_VALUE) {
            Sleep(2000);
            continue;
        }
        plog(std::string("input: DualSense") + (edge ? " Edge" : "") + " read directly over " + (bt ? "Bluetooth" : "USB"));
        T = DsState{};
        OVERLAPPED ov{};
        ov.hEvent    = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool pending = false;
        BYTE buf[256];
        for (;;) {
            if (!pending) {
                ResetEvent(ov.hEvent);
                if (!ReadFile(h, buf, in_len, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
                pending = true;
            }
            const DWORD w = WaitForSingleObject(ov.hEvent, 500);
            if (w == WAIT_TIMEOUT) continue;
            DWORD n = 0;
            if (w != WAIT_OBJECT_0 || !GetOverlappedResult(h, &ov, &n, FALSE)) break;
            pending = false;
            Virt v;
            hid_parse(buf, n, bt, edge, v);
            if (v.present) {
                std::scoped_lock lk{g_hmu};
                v.packet = g_hid.packet + 1;
                g_hid    = v;
                g_hid_present.store(true, std::memory_order_relaxed);
            }
        }
        CancelIo(h);
        CloseHandle(h);
        CloseHandle(ov.hEvent);
        {
            std::scoped_lock lk{g_hmu};
            g_hid = Virt{};
            g_hid_present.store(false, std::memory_order_relaxed);
        }
        plog("input: the DualSense read directly is gone");
        Sleep(1000);
    }
}

// ---- the XInputGetState hook ------------------------------------------------------------------------------------------
using GetStateFn = DWORD(WINAPI*)(DWORD, PadState*);
GetStateFn g_real = nullptr; // MinHook's trampoline to the real XInputGetState

HMODULE g_self = nullptr;
std::wstring g_mods_dir; // lower case, with a trailing backslash
std::wstring g_game_dir; // the Azimuth folder, lower case, with a trailing backslash
std::mutex g_cmu;
std::unordered_map<HMODULE, bool> g_caller_cache;

void note_self() {
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&note_self), &g_self);
    std::wstring d = (azimuth_dir() / "fh3-mods").wstring();
    std::transform(d.begin(), d.end(), d.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    g_mods_dir = d;
    std::wstring g = azimuth_dir().wstring();
    std::transform(g.begin(), g.end(), g.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (!g.empty() && g.back() != L'\\') g += L'\\';
    g_game_dir = g;
}

// A mod (a DLL in fh3-mods) or the platform itself?
bool caller_is_mod(void* ra) {
    HMODULE h = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(ra), &h) || !h)
        return false;
    if (h == g_self) return true;
    {
        std::scoped_lock lk{g_cmu};
        auto it = g_caller_cache.find(h);
        if (it != g_caller_cache.end()) return it->second;
    }
    wchar_t path[MAX_PATH * 2] = L"";
    bool mod                   = false;
    if (GetModuleFileNameW(h, path, MAX_PATH * 2)) {
        std::wstring p = path;
        std::transform(p.begin(), p.end(), p.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        mod = p.size() > g_mods_dir.size() && p.compare(0, g_mods_dir.size(), g_mods_dir) == 0;
        // Other DLLs that sit next to azimuth.exe (Extra Options' azimuth-tls.dll reads XInput directly for the minimap's
        // right-stick rotation and the garage's View button) get the controller too; the game's own executables do not.
        if (!mod && h != GetModuleHandleW(nullptr) && p.size() > g_game_dir.size() && p.compare(0, g_game_dir.size(), g_game_dir) == 0 &&
            p.size() > 4 && p.compare(p.size() - 4, 4, L".dll") == 0) {
            const std::wstring rest = p.substr(g_game_dir.size());
            mod = rest.find(L'\\') == std::wstring::npos || rest.find(L"azimuth") != std::wstring::npos;
        }
    }
    std::scoped_lock lk{g_cmu};
    g_caller_cache[h] = mod;
    return mod;
}

Virt combine() {
    Virt v;
    auto bigger = [](SHORT cur, SHORT nw) { return std::abs(static_cast<int>(nw)) > std::abs(static_cast<int>(cur)) ? nw : cur; };
    auto add    = [&](const Virt& o) {
        if (!o.present) return;
        v.present = true;
        v.buttons = static_cast<WORD>(v.buttons | o.buttons);
        v.lt      = std::max(v.lt, o.lt);
        v.rt      = std::max(v.rt, o.rt);
        v.lx = bigger(v.lx, o.lx); v.ly = bigger(v.ly, o.ly); v.rx = bigger(v.rx, o.rx); v.ry = bigger(v.ry, o.ry);
        v.packet += o.packet;
    };
    if (g_present.load(std::memory_order_relaxed)) {
        std::scoped_lock lk{g_vmu};
        add(g_virt);
    }
    if (g_hid_present.load(std::memory_order_relaxed)) {
        std::scoped_lock lk{g_hmu};
        add(g_hid);
    }
    add(kb_poll());
    return v;
}

DWORD WINAPI detour_get_state(DWORD idx, PadState* s) {
    const DWORD r = g_real(idx, s);
    if (!s || idx > 3 || !(g_present.load(std::memory_order_relaxed) || g_hid_present.load(std::memory_order_relaxed) || g_kb_joined.load())) return r;
    if (r != ERROR_SUCCESS && idx != 0) return r;
    if (!caller_is_mod(__builtin_return_address(0))) return r;
    const Virt v = combine();
    if (!v.present) return r;
    if (r == ERROR_SUCCESS) { // an XInput pad is there: the other controller's input joins it
        s->buttons = static_cast<WORD>(s->buttons | v.buttons);
        s->lt      = std::max(s->lt, v.lt);
        s->rt      = std::max(s->rt, v.rt);
        auto bigger = [](SHORT cur, SHORT nw) { return std::abs(static_cast<int>(nw)) > std::abs(static_cast<int>(cur)) ? nw : cur; };
        s->lx      = bigger(s->lx, v.lx);
        s->ly      = bigger(s->ly, v.ly);
        s->rx      = bigger(s->rx, v.rx);
        s->ry      = bigger(s->ry, v.ry);
        s->packet += v.packet;
        return r;
    }
    *s = PadState{v.packet, v.buttons, v.lt, v.rt, v.lx, v.ly, v.rx, v.ry}; // no XInput pad: the other controller is pad 1
    return ERROR_SUCCESS;
}


// ---- the controller as one of the engine's gamepads ---------------------------------------------------------------------------
// Azimuth gives the game only the PC's Windows.Gaming.Input gamepads (Xbox-like pads), so a DualSense or any other HID pad that is
// not presented as an Xbox 360 pad does nothing in the game at all. The engine takes the Gamepad statics, registers a
// GamepadAdded and a GamepadRemoved handler, and reads every pad it was given with GetCurrentReading (and sends it the game's
// rumble with put_Vibration). So the other controller is made a gamepad object of our own and handed to the engine's own
// GamepadAdded handler (found in azimuth.exe by the code that registers it, as FH2-Input does); it leaves through GamepadRemoved.
// It only joins when XInput has no pad of its own (the game already has that one), and it carries the game's rumble back to
// the mods through the shared memory "Local\FH3ModPlatform.Vibration" (4 floats: left motor, right motor, left trigger,
// right trigger, then a tick count).
struct WgiReading {
    UINT64 timestamp;
    UINT32 buttons;
    double lt, rt, lx, ly, rx, ry;
};
struct WgiVibration {
    double lm, rm, lt, rt;
};
enum : UINT32 { W_MENU = 1, W_VIEW = 2, W_A = 4, W_B = 8, W_X = 0x10, W_Y = 0x20, W_UP = 0x40, W_DOWN = 0x80, W_LEFT = 0x100,
                W_RIGHT = 0x200, W_LB = 0x400, W_RB = 0x800, W_LS = 0x1000, W_RS = 0x2000 };

const IID kIidUnknown   = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const IID kIidInspectable = {0xAF86E2E0, 0xB12D, 0x4C6A, {0x9C, 0x5A, 0xD7, 0xAA, 0x65, 0x10, 0x1E, 0x90}};
const IID kIidAgile     = {0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};
const IID kIidGamepad   = {0xBC7BB43C, 0x0A69, 0x3903, {0x9E, 0x9D, 0xA5, 0x0F, 0x86, 0xA4, 0x5D, 0xE5}};

struct WgiPad;
struct WgiPadVtbl {
    HRESULT(STDMETHODCALLTYPE* QueryInterface)(WgiPad*, REFIID, void**);
    ULONG(STDMETHODCALLTYPE* AddRef)(WgiPad*);
    ULONG(STDMETHODCALLTYPE* Release)(WgiPad*);
    HRESULT(STDMETHODCALLTYPE* GetIids)(WgiPad*, ULONG*, IID**);
    HRESULT(STDMETHODCALLTYPE* GetRuntimeClassName)(WgiPad*, void**);
    HRESULT(STDMETHODCALLTYPE* GetTrustLevel)(WgiPad*, int*);
    HRESULT(STDMETHODCALLTYPE* get_Vibration)(WgiPad*, WgiVibration*);
    HRESULT(STDMETHODCALLTYPE* put_Vibration)(WgiPad*, const WgiVibration*);
    HRESULT(STDMETHODCALLTYPE* GetCurrentReading)(WgiPad*, WgiReading*);
};
struct WgiPad {
    const WgiPadVtbl* v;
    volatile LONG refs;
};

WgiVibration g_vib{};
std::mutex g_wmu;

HRESULT STDMETHODCALLTYPE pad_qi(WgiPad* p, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    if (IsEqualGUID(iid, kIidUnknown) || IsEqualGUID(iid, kIidInspectable) || IsEqualGUID(iid, kIidAgile) || IsEqualGUID(iid, kIidGamepad)) {
        InterlockedIncrement(&p->refs);
        *out = p;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE pad_addref(WgiPad* p) { return static_cast<ULONG>(InterlockedIncrement(&p->refs)); }
ULONG STDMETHODCALLTYPE pad_release(WgiPad* p) { return static_cast<ULONG>(InterlockedDecrement(&p->refs)); }
HRESULT STDMETHODCALLTYPE pad_iids(WgiPad*, ULONG* n, IID** iids) {
    if (n) *n = 0;
    if (iids) *iids = nullptr;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE pad_class(WgiPad*, void** name) {
    if (name) *name = nullptr;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE pad_trust(WgiPad*, int* level) {
    if (level) *level = 0;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE pad_get_vibration(WgiPad*, WgiVibration* v) {
    if (!v) return E_POINTER;
    std::scoped_lock lk{g_wmu};
    *v = g_vib;
    return S_OK;
}
float unit01(double v) { return v > 0 && v <= 1 ? static_cast<float>(v) : v > 1 ? 1.0f : 0.0f; }
HRESULT STDMETHODCALLTYPE pad_put_vibration(WgiPad*, const WgiVibration* v) {
    if (!v) return E_POINTER;
    {
        std::scoped_lock lk{g_wmu};
        g_vib = *v;
    }
    if (g_shared) {
        g_shared->lm   = unit01(v->lm);
        g_shared->rm   = unit01(v->rm);
        g_shared->lt   = unit01(v->lt);
        g_shared->rt   = unit01(v->rt);
        g_shared->tick = GetTickCount();
    }
    return S_OK;
}
double axis_d(SHORT v) { return v >= 0 ? v / 32767.0 : v / 32768.0; }
HRESULT STDMETHODCALLTYPE pad_reading(WgiPad*, WgiReading* r) {
    if (!r) return E_POINTER;
    static const struct { WORD x; UINT32 w; } map[14] = {
        {X_UP, W_UP}, {X_DOWN, W_DOWN}, {X_LEFT, W_LEFT}, {X_RIGHT, W_RIGHT}, {X_START, W_MENU}, {X_BACK, W_VIEW}, {X_LS, W_LS},
        {X_RS, W_RS}, {X_LB, W_LB}, {X_RB, W_RB}, {X_A, W_A}, {X_B, W_B}, {X_X, W_X}, {X_Y, W_Y}};
    const Virt v = combine();
    std::memset(r, 0, sizeof *r);
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    r->timestamp = static_cast<UINT64>(static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart) * 1e6);
    for (const auto& m : map)
        if (v.buttons & m.x) r->buttons |= m.w;
    r->lt = v.lt / 255.0;
    r->rt = v.rt / 255.0;
    r->lx = axis_d(v.lx);
    r->ly = axis_d(v.ly);
    r->rx = axis_d(v.rx);
    r->ry = axis_d(v.ry);
    return S_OK;
}
const WgiPadVtbl kPadVtbl = {pad_qi, pad_addref, pad_release, pad_iids, pad_class, pad_trust, pad_get_vibration, pad_put_vibration, pad_reading};
WgiPad g_pad              = {&kPadVtbl, 1};

BYTE *g_eng_added = nullptr, *g_eng_removed = nullptr;
void* volatile* g_eng_statics = nullptr;
bool g_pad_in = false;
bool g_found_logged = false;

struct Image {
    BYTE *base = nullptr, *text = nullptr, *text_end = nullptr, *rdata = nullptr, *rdata_end = nullptr, *data = nullptr, *data_end = nullptr;
};
bool image_of(BYTE* b, Image* m) {
    if (!b || reinterpret_cast<IMAGE_DOS_HEADER*>(b)->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(b + reinterpret_cast<IMAGE_DOS_HEADER*>(b)->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    m->base                 = b;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s) {
        if (!std::memcmp(s->Name, ".text", 6)) { m->text = b + s->VirtualAddress; m->text_end = m->text + s->Misc.VirtualSize; }
        else if (!std::memcmp(s->Name, ".rdata", 7)) { m->rdata = b + s->VirtualAddress; m->rdata_end = m->rdata + s->Misc.VirtualSize; }
        else if (!std::memcmp(s->Name, ".data", 6)) { m->data = b + s->VirtualAddress; m->data_end = m->data + s->Misc.VirtualSize; }
    }
    return m->text && m->rdata && m->data;
}
BYTE* rel32(BYTE* insn, int disp_at, int len) { return insn + len + *reinterpret_cast<INT32*>(insn + disp_at); }
bool in_range(const void* p, const BYTE* lo, const BYTE* hi) { return static_cast<const BYTE*>(p) >= lo && static_cast<const BYTE*>(p) < hi; }
bool handler_ok(Image* m, BYTE* h, BYTE kind) {
    if (!in_range(h, m->data, m->data_end - 16)) return false;
    void** vt = *reinterpret_cast<void***>(h);
    return in_range(vt, m->rdata, m->rdata_end - 32) && in_range(vt[3], m->text, m->text_end) && h[8] == kind;
}

// The engine's two handlers and its statics: after "lea rcx, [L"Windows.Gaming.Input.Gamepad"]" come the calls of the statics'
// add_GamepadAdded (vtable +0x30) and add_GamepadRemoved (+0x40), each with "lea rdx, [handler]" before it, and the statics are
// stored ("mov [rip+x], rax"). The handler objects get their vtable when the engine starts, so this is tried again until they have.
bool wgi_find_engine(BYTE* engine) {
    static const wchar_t cls[] = L"Windows.Gaming.Input.Gamepad";
    Image m;
    if (!image_of(engine, &m)) return false;
    BYTE *str = nullptr, *lea = nullptr;
    for (BYTE* p = m.rdata + 2; p + sizeof cls <= m.rdata_end && !str; p += 2)
        if (*reinterpret_cast<const wchar_t*>(p) == L'W' && !std::memcmp(p, cls, sizeof cls)) str = p;
    for (BYTE* p = m.text; str && p + 7 <= m.text_end && !lea; ++p)
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x0D && rel32(p, 3, 7) == str) lea = p;
    BYTE *added = nullptr, *removed = nullptr;
    void* volatile* statics = nullptr;
    for (BYTE* p = lea; p && p < lea + 0x140 && p + 16 <= m.text_end && !statics; ++p) {
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x15) {
            for (BYTE* q = p + 7; q < p + 16; ++q)
                if (q[0] == 0xFF && q[1] == 0x50 && (q[2] == 0x30 || q[2] == 0x40)) {
                    if (q[2] == 0x30 && !added) added = rel32(p, 3, 7);
                    else if (q[2] == 0x40 && !removed) removed = rel32(p, 3, 7);
                    break;
                }
        } else if (added && removed && p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x05) {
            statics = reinterpret_cast<void* volatile*>(rel32(p, 3, 7));
        }
    }
    if (!statics || !in_range(const_cast<void**>(statics), m.data, m.data_end) || !handler_ok(&m, added, 1) || !handler_ok(&m, removed, 0) ||
        *reinterpret_cast<void**>(added) != *reinterpret_cast<void**>(removed))
        return false;
    g_eng_added = added; g_eng_removed = removed; g_eng_statics = statics;
    plog("input: the engine's GamepadAdded/Removed handlers found (azimuth.exe+0x" + [&] { char b[40]; std::snprintf(b, sizeof b, "%lX/0x%lX", static_cast<unsigned long>(added - m.base), static_cast<unsigned long>(removed - m.base)); return std::string(b); }() + ")");
    return true;
}

using WgiInvokeFn = HRESULT(STDMETHODCALLTYPE*)(void*, void*, void*);
HRESULT wgi_invoke(bool in) {
    BYTE* h = in ? g_eng_added : g_eng_removed;
    __try {
        return reinterpret_cast<WgiInvokeFn>((*reinterpret_cast<void***>(h))[3])(h, nullptr, &g_pad);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return E_FAIL;
    }
}

bool real_xinput_pad() {
    for (DWORD i = 0; i < 4; ++i) {
        PadState st{};
        if (g_real(i, &st) == ERROR_SUCCESS) return true;
    }
    return false;
}

DWORD WINAPI wgi_thread(LPVOID) {
    ULONGLONG next_real = 0;
    bool real           = false;
    for (;;) {
        Sleep(100);
        BYTE* engine = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        if (!g_eng_added) {
            static ULONGLONG last_try = 0;
            const ULONGLONG t         = GetTickCount64();
            if (t - last_try < 1000) continue;
            last_try = t;
            if (!wgi_find_engine(engine)) {
                if (!g_found_logged && t > 120000) {
                    g_found_logged = true;
                    plog("input: Azimuth's gamepad code isn't as expected (or isn't up yet): other controllers can't be given to the game");
                }
                continue;
            }
        }
        const ULONGLONG t = GetTickCount64();
        if (t >= next_real) {
            next_real = t + 500;
            real      = real_xinput_pad();
        }
        const bool want = (!real && (g_present.load(std::memory_order_relaxed) || g_hid_present.load(std::memory_order_relaxed))) ||
                         (g_kb_on && (g_kb_joined.load() || kb_any()));
        if (want == g_pad_in) continue;
        if (want && !*g_eng_statics) continue; // the engine's pads aren't up yet
        const HRESULT hr = wgi_invoke(want);
        g_pad_in         = want;
        char b[96];
        std::snprintf(b, sizeof b, "0x%08lX", static_cast<unsigned long>(hr));
        plog(std::string("input: the controller ") + (want ? "joins" : "leaves") + " the game's gamepads (" + b + ")");
    }
}

bool setting(const wchar_t* key, bool def) {
    const std::wstring ini = ini_path();
    return GetPrivateProfileIntW(L"input", key, def ? 1 : 0, ini.c_str()) != 0;
}

} // namespace

// Which device is in use: an XInput pad's real input (a button, a trigger past a quarter, a stick past a third) is timed here, the
// DualSense mod times the DualSense's; the later of the two is the device in use.
DWORD WINAPI xbox_activity_thread(LPVOID) {
    DWORD last_packet[4] = {};
    bool on[4]           = {};
    DWORD rescan         = 0;
    for (;;) {
        Sleep(30);
        if (!g_real || !g_shared) continue;
        const DWORD now = GetTickCount();
        const bool scan = now - rescan > 1500;
        if (scan) rescan = now;
        for (DWORD i = 0; i < 4; ++i) {
            if (!on[i] && !scan) continue;
            PadState st{};
            on[i] = g_real(i, &st) == ERROR_SUCCESS;
            if (!on[i]) continue;
            if (st.packet == last_packet[i]) continue;
            last_packet[i] = st.packet;
            const bool act = (st.buttons & 0xF3FF) || st.lt > 64 || st.rt > 64 || std::abs(st.lx) > 11000 || std::abs(st.ly) > 11000 ||
                             std::abs(st.rx) > 11000 || std::abs(st.ry) > 11000;
            if (act) g_shared->xbox_tick = now ? now : 1;
        }
    }
}

void start_input() {
    if (!setting(L"Enabled", true)) {
        plog("input: turned off (platform.ini [input] Enabled=0)");
        return;
    }
    note_self();
    HMODULE x = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    void* target = x ? reinterpret_cast<void*>(GetProcAddress(x, "XInputGetState")) : nullptr;
    if (!target) {
        plog("input: xinput1_4.dll isn't available: the mods only see what Windows gives them");
        return;
    }
    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) {
        plog(std::string("input: MinHook didn't start: ") + MH_StatusToString(init));
        return;
    }
    void* tramp  = nullptr;
    MH_STATUS st = MH_CreateHook(target, reinterpret_cast<void*>(&detour_get_state), &tramp);
    if (st == MH_OK) {
        g_real = reinterpret_cast<GetStateFn>(tramp);
        st     = MH_EnableHook(target);
    }
    if (st != MH_OK) {
        plog(std::string("input: couldn't hook XInputGetState: ") + MH_StatusToString(st));
        return;
    }
    plog("input: XInputGetState is seen by the mods with every controller added (the game's own input is untouched)");
    if (!setting(L"Raw", true)) {
        plog("input: the controller reader is off (platform.ini [input] Raw=0)");
        return;
    }
    read_ignore();
    if (HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 64, L"Local\\FH3ModPlatform.Vibration"))
        g_shared = static_cast<Shared*>(MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 64));
    load_controller_settings();
    load_kb_settings();
    if (g_ms_on)
        if (HANDLE t = CreateThread(nullptr, 0, ms_thread, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, xbox_activity_thread, nullptr, 0, nullptr)) CloseHandle(t);
    if (setting(L"Gamepad", true)) {
        if (HANDLE t = CreateThread(nullptr, 0, wgi_thread, nullptr, 0, nullptr)) CloseHandle(t);
    } else {
        plog("input: the controller is not given to the game (platform.ini [input] Gamepad=0)");
    }
    if (HANDLE t = CreateThread(nullptr, 0, hid_reader, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, reader, nullptr, 0, nullptr)) CloseHandle(t);
}

} // namespace fh3platform
