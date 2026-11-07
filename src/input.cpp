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


// ---- a DualSense read straight from its HID device (USB and Bluetooth) -------------------------------------------------------
// Over Bluetooth the controller sends the short report 0x01 until something asks for the full one (a program lighting it up,
// FH2-Input, Steam); from then on it sends 0x31 only, and Windows' own gamepad list shows nothing of it. Reading the device
// ourselves (a handle of its own: the game and every other program keep theirs) sees both, and USB too.
bool is_ds(USHORT vid, USHORT pid) { return vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2); }

HANDLE hid_open(USHORT* in_len, bool* bt) {
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

void hid_parse(const BYTE* b, DWORD n, bool bt, Virt& v) {
    // the same fields in three layouts: USB 0x01 and Bluetooth 0x31 (X Y RX RY L2 R2 seq buttons0..2), Bluetooth 0x01 (short)
    BYTE x, y, rx, ry, l2, r2, b0, b1;
    if (b[0] == 0x31 && n >= 12) {
        const BYTE* d = b + 2;
        x = d[0]; y = d[1]; rx = d[2]; ry = d[3]; l2 = d[4]; r2 = d[5]; b0 = d[7]; b1 = d[8];
    } else if (b[0] == 0x01 && !bt && n >= 11) {
        const BYTE* d = b + 1;
        x = d[0]; y = d[1]; rx = d[2]; ry = d[3]; l2 = d[4]; r2 = d[5]; b0 = d[7]; b1 = d[8];
    } else if (b[0] == 0x01 && bt && n >= 10) {
        x = b[1]; y = b[2]; rx = b[3]; ry = b[4]; b0 = b[5]; b1 = b[6]; l2 = b[8]; r2 = b[9];
    } else {
        return;
    }
    WORD btn = 0;
    if (b0 & 0x10) btn |= X_X;
    if (b0 & 0x20) btn |= X_A;
    if (b0 & 0x40) btn |= X_B;
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
    if (b1 & 0x10) btn |= X_BACK;
    if (b1 & 0x20) btn |= X_START;
    if (b1 & 0x40) btn |= X_LS;
    if (b1 & 0x80) btn |= X_RS;
    auto st = [](BYTE c) { return static_cast<SHORT>((static_cast<int>(c) - 128) * 257); };
    v.buttons = btn;
    v.lt = l2;
    v.rt = r2;
    if (bt && b[0] == 0x01) { // the short report's triggers can be missing: the digital ones then
        if (!l2 && (b1 & 0x04)) v.lt = 255;
        if (!r2 && (b1 & 0x08)) v.rt = 255;
    }
    v.lx = st(x);
    v.ly = static_cast<SHORT>(-st(y));
    v.rx = st(rx);
    v.ry = static_cast<SHORT>(-st(ry));
    v.present = true;
}

DWORD WINAPI hid_reader(LPVOID) {
    for (;;) {
        USHORT in_len = 0;
        bool bt       = false;
        HANDLE h      = hid_open(&in_len, &bt);
        if (h == INVALID_HANDLE_VALUE) {
            Sleep(2000);
            continue;
        }
        plog(std::string("input: DualSense read directly over ") + (bt ? "Bluetooth" : "USB"));
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
            hid_parse(buf, n, bt, v);
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
std::mutex g_cmu;
std::unordered_map<HMODULE, bool> g_caller_cache;

void note_self() {
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&note_self), &g_self);
    std::wstring d = (azimuth_dir() / "fh3-mods").wstring();
    std::transform(d.begin(), d.end(), d.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (!d.empty() && d.back() != L'\\') d += L'\\';
    g_mods_dir = d;
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
    return v;
}

DWORD WINAPI detour_get_state(DWORD idx, PadState* s) {
    const DWORD r = g_real(idx, s);
    if (!s || idx > 3 || !(g_present.load(std::memory_order_relaxed) || g_hid_present.load(std::memory_order_relaxed))) return r;
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

bool setting(const wchar_t* key, bool def) {
    const std::wstring ini = ini_path();
    return GetPrivateProfileIntW(L"input", key, def ? 1 : 0, ini.c_str()) != 0;
}

} // namespace

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
    if (HANDLE t = CreateThread(nullptr, 0, hid_reader, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, reader, nullptr, 0, nullptr)) CloseHandle(t);
}

} // namespace fh3platform
