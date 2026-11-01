// The MODS tile and the Exit to Desktop tile on the Home tab (FH3).
//
// MODS: the Home tab's Horizon Group tile (PART_Button_GameParty) needs the online services, so offline the game
// locks it. The platform turns it into the MODS tile:
//   - ui_patches.cpp names it MODS in A_PauseMenu.xaml and has pause.xml (the state flow) send its
//     "online_party_list" transition to the platform's page instead of the party list;
//   - tile states are set through game+0x16A4CF0 (page, name, state: 0 shown, 1 hidden, 2 locked) and
//     game+0x1695B90 (page, name, reason: locks the tile, +0x5E0 with 2, and stores the reason). For this tile the
//     first always gets 0 and the second is skipped;
//   - its click handlers (game+0x157AFD0 (page) and 0x157B060 (page, int)) are replaced: the game's show the locked
//     popup (0x16A9A80) or check the online services; the platform's fire the transition straight away with
//     0x1689F60(page, &TransitionId, 0), the id at game+0x4E31B18 being "online_party_list".
// Exit to Desktop: the tile only has a Command binding, and the command has no delegate in this build. The game
// wires the Home tab's click handlers in 0x1585690 with 0x1553850(page, name, &{fn, adjust}); after it, the platform
// adds one for PART_Button_ExitToDesktop (name pointer at game+0x4586788) that ends the game.
#include "platform.hpp"

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>

namespace fh3platform {
namespace {

constexpr uint32_t RVA_HANDLER_A   = 0x157AFD0;
constexpr uint32_t RVA_HANDLER_B   = 0x157B060;
constexpr uint32_t RVA_NAME        = 0x45866C0;  // const char* const: "PART_Button_GameParty"
constexpr uint32_t RVA_SET_STATE_A = 0x16A4CF0;
constexpr uint32_t RVA_SET_STATE_B = 0x1695B90;
constexpr uint32_t RVA_FIRE        = 0x1689F60;
constexpr uint32_t RVA_PARTY_ID    = 0x4E31B18;
constexpr uint32_t RVA_WIRE        = 0x1585690;
constexpr uint32_t RVA_ON_CLICK    = 0x1553850;
constexpr uint32_t RVA_EXIT_NAME   = 0x4586788;  // const char* const: "PART_Button_ExitToDesktop"

using SetState = void (*)(void*, const void*, int);
using Fire     = void (*)(void*, const void*, int);
using Wire     = void (*)(void*);
using OnClick  = void (*)(void*, const void*, const void*);

SetState g_set_a = nullptr;
SetState g_set_b = nullptr;
Wire g_wire      = nullptr;
std::atomic<int> g_logs{0};

char* game() { return static_cast<char*>(find_game()); }
void once(const std::string& line) {
    if (g_logs.fetch_add(1) < 12) plog(line);
}

bool is_mods_tile(const void* name) {
    char* base = game();
    if (!base) return false;
    if (name == *reinterpret_cast<const void* const*>(base + RVA_NAME)) return true;
    return false;
}
std::atomic<void*> g_home{nullptr};   // the Home tab page the MODS tile was pressed on
std::atomic<int> g_reopen{0};          // a Mods page to open as soon as the Home tab is back (see mods_reopen)
std::atomic<uint64_t> g_reopen_at{0};
void open_mods(void* page);
void set_state_a(void* page, const void* name, int state) {
    if (is_mods_tile(name)) state = 0;
    g_set_a(page, name, state);
}
void set_state_b(void* page, const void* name, int reason) {

    if (is_mods_tile(name)) return;
    g_set_b(page, name, reason);
}

void open_mods(void* page) {
    char* base = game();
    if (!base || !page) return;
    g_home = page;
    once(settings_hub_open(1) ? "MODS tile pressed: opening Installed mods"
                              : "MODS tile pressed, but the Mods page isn't ready (see above): the HUD options page opens");
    reinterpret_cast<Fire>(base + RVA_FIRE)(page, base + RVA_PARTY_ID, 0);
}
}  // namespace

// From a Mods page: leave it (the caller does, with the game's own back) and open `which` as soon as the Home tab
// is back and sets its tiles.
void mods_reopen(int which) {
    g_reopen_at = GetTickCount64() + 250;
    g_reopen    = which;
    // if the Home tab sets no tile in the meantime, open it from here after a moment
    std::thread([which] {
        Sleep(900);
        int w = which;
        if (!g_reopen.compare_exchange_strong(w, 0)) return;
        plog("reopen: no tile was set, opening from the timer");
        void* page = g_home.load();
        char* base = game();
        if (!page || !base || !settings_hub_open(which)) return;
        __try {
            reinterpret_cast<Fire>(base + RVA_FIRE)(page, base + RVA_PARTY_ID, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            plog("reopen: the transition faulted");
        }
    }).detach();
}

namespace {
void open_hub(void* page, int which) {
    char* base = game();
    if (!base || !page) return;
    g_home = page;
    settings_hub_open(which);
    reinterpret_cast<Fire>(base + RVA_FIRE)(page, base + RVA_PARTY_ID, 0);
}
void tab_installed(void* page) { open_hub(page, 1); }
void tab_updates(void* page) { open_hub(page, 2); }
void tab_about(void* page) { open_hub(page, 4); }
void tab_all(void* page) { open_hub(page, 6); }
void tab_get(void*) {}  // no website yet
void handler_a(void* page) { open_mods(page); }
void handler_b(void* page, int) { open_mods(page); }

// The confirmation: the game's message box, as the pause menu's own Yes/No popups build it (the Tombola spin at
// 0x157B630): box = 0x13FDEE0(page); title/body = 0x1CA7BC0(&holder, table, string id) then 0x1617570 / 0x1616EE0
// (box, &holder); buttons = 0x1598950(box, 4) (4 is Yes/No); 0x1598510(box, 0, &std::function, true) sets what Yes
// does and shows it (No just closes). The texts are Main.IDS_ExitGame_Title/Desc ("Exit Game", "Are you certain you
// want to exit the game?"), ids at game+0x4E57588 / 0x4E5758C; "Main" at game+0x2D4F6B0.
constexpr uint32_t RVA_MSGBOX      = 0x13FDEE0;
constexpr uint32_t RVA_LOC         = 0x1CA7BC0;
constexpr uint32_t RVA_SET_TITLE   = 0x1617570;
constexpr uint32_t RVA_SET_BODY    = 0x1616EE0;
constexpr uint32_t RVA_BUTTONS     = 0x1598950;
constexpr uint32_t RVA_ON_BUTTON   = 0x1598510;
constexpr uint32_t RVA_TABLE_MAIN  = 0x2D4F6B0;
constexpr uint32_t RVA_EXIT_TITLE  = 0x4E57588;
constexpr uint32_t RVA_EXIT_DESC   = 0x4E5758C;

using GetBox  = void* (*)(void*);
using Loc     = void** (*)(void**, const char*, int);
using SetText = void (*)(void*, void**);
using Buttons = void (*)(void*, int);
using OnButton = void (*)(void*, int, void*, bool);

// An MSVC std::function<void()> the game can copy: 0x38 bytes of storage, then the pointer to the callable; ours
// lives in the storage and is just a vtable (_Copy, _Move, _Do_call, _Target_type, _Delete_this, _Get).
void* fn_copy(void* self, void* where);
// Saving, as the state flow's SaveProfile behavior does it (game+0xD80B80): the game object (pointer at
// game+0x4D6FC00), its vtable +0x48 gives the profile side, whose +0xE8 gives the saver; game+0x334390(saver, force)
// is the blocking save (0x3343A0 the async one).
constexpr uint32_t RVA_GAME_OBJECT = 0x4D6FC00;
constexpr uint32_t RVA_SAVE_NOW    = 0x334390;

bool save_now() {
    char* base = game();
    if (!base) return false;
    __try {
        void* g = *reinterpret_cast<void**>(base + RVA_GAME_OBJECT);
        if (!g) return false;
        void* profile = reinterpret_cast<void* (*)(void*)>((*static_cast<void***>(g))[0x48 / 8])(g);
        if (!profile) return false;
        void* saver = reinterpret_cast<void* (*)(void*)>((*static_cast<void***>(profile))[0xE8 / 8])(profile);
        if (!saver) return false;
        reinterpret_cast<void (*)(void*, bool)>(base + RVA_SAVE_NOW)(saver, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// After Yes the state flow takes over (ui_patches.cpp): the pause page's "online_public_custom_roadtrip" transition
// (TransitionId at game+0x4E318E8; online only, so free here) leads to the saving screen, the game's blocking save
// (SaveProfile, game+0x3343B0 underneath, hooked here) and holds the player there. When that save returns, the
// platform waits a moment and ends the game. (A TransitionId made at run time does nothing: the state flow resolves
// transition names against the ones made at start-up.)
constexpr uint32_t RVA_EXIT_FLOW_ID = 0x4E318E8;
constexpr uint32_t RVA_SAVE         = 0x3343B0;  // (saver, bool blocking, bool force)
using Save = void (*)(void*, bool, bool);
Save g_save = nullptr;
std::atomic<bool> g_exit_requested{false};
std::atomic<bool> g_exit_scheduled{false};
std::atomic<void*> g_exit_page{nullptr};

void end_game_soon(const char* why, int ms) {
    bool expected = false;
    if (!g_exit_scheduled.compare_exchange_strong(expected, true)) return;
    plog(why);
    std::thread([ms] {
        Sleep(ms);
        TerminateProcess(GetCurrentProcess(), 0);
    }).detach();
}

void save_detour(void* saver, bool blocking, bool force) {
    g_save(saver, blocking, force);
    if (g_exit_requested.load() && blocking)
        end_game_soon("Exit to Desktop: the save is done; ending the game process", 1500);
}

bool fire_exit_flow() {
    char* base  = game();
    void* page  = g_exit_page.load();
    if (!base || !page || !g_save) return false;
    __try {
        reinterpret_cast<Fire>(base + RVA_FIRE)(page, base + RVA_EXIT_FLOW_ID, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* fn_do(void*) {
    g_exit_requested.store(true);
    if (fire_exit_flow()) {
        plog("Exit to Desktop confirmed: showing the saving screen and saving");
        // if the flow's save never shows up, save here and leave anyway
        std::thread([] {
            Sleep(20000);
            if (!g_exit_scheduled.load()) {
                plog("Exit to Desktop: no save seen within 20 seconds");
                end_game_soon("Exit to Desktop: ending the game process", 0);
            }
        }).detach();
        return nullptr;
    }
    const bool saved = save_now();
    end_game_soon(saved ? "Exit to Desktop confirmed: saved here (the saving screen couldn't be shown); ending the game process"
                        : "Exit to Desktop confirmed: the save couldn't be started; ending the game process",
                  3000);
    return nullptr;
}
void* fn_type(void*) { return nullptr; }
void fn_delete(void*, bool) {}
void* fn_get(void* self) { return self; }
void* g_fn_vtable[] = {reinterpret_cast<void*>(&fn_copy), reinterpret_cast<void*>(&fn_copy),
                       reinterpret_cast<void*>(&fn_do),   reinterpret_cast<void*>(&fn_type),
                       reinterpret_cast<void*>(&fn_delete), reinterpret_cast<void*>(&fn_get)};
void* fn_copy(void*, void* where) {
    if (where) *static_cast<void***>(where) = g_fn_vtable;
    return where;
}
struct GameFunction {
    void* storage[7];
    void* impl;
};

void release(void* holder_value) {
    if (!holder_value) return;
    void** vt = *static_cast<void***>(holder_value);
    reinterpret_cast<void (*)(void*)>(vt[3])(holder_value); // +0x18
}

bool show_exit_confirm(void* page) {
    char* base = game();
    if (!base || !page) return false;
    __try {
        void* box = reinterpret_cast<GetBox>(base + RVA_MSGBOX)(page);
        if (!box) return false;
        const char* table = base + RVA_TABLE_MAIN;
        void* title = nullptr;
        void* body  = nullptr;
        reinterpret_cast<Loc>(base + RVA_LOC)(&title, table, *reinterpret_cast<int*>(base + RVA_EXIT_TITLE));
        reinterpret_cast<SetText>(base + RVA_SET_TITLE)(box, &title);
        release(title);
        reinterpret_cast<Loc>(base + RVA_LOC)(&body, table, *reinterpret_cast<int*>(base + RVA_EXIT_DESC));
        reinterpret_cast<SetText>(base + RVA_SET_BODY)(box, &body);
        release(body);
        reinterpret_cast<Buttons>(base + RVA_BUTTONS)(box, 4);
        GameFunction yes{};
        yes.storage[0] = g_fn_vtable;
        yes.impl       = &yes.storage[0];
        reinterpret_cast<OnButton>(base + RVA_ON_BUTTON)(box, 0, &yes, true);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void exit_click(void* page) {
    g_exit_page.store(page);
    if (!show_exit_confirm(page)) {
        plog("Exit to Desktop: the confirmation popup couldn't be shown; staying in the game");
    }
}
struct PMF {
    void* fn;
    int adjust;
};
const PMF g_exit_pmf{reinterpret_cast<void*>(&exit_click), 0};

void wire(void* page) {
    g_wire(page);
    char* base = game();
    if (!base || !page) return;
    const void* name = *reinterpret_cast<const void* const*>(base + RVA_EXIT_NAME);
    reinterpret_cast<OnClick>(base + RVA_ON_CLICK)(page, name, &g_exit_pmf);
    static const PMF pmf[] = {{reinterpret_cast<void*>(&tab_installed), 0}, {reinterpret_cast<void*>(&tab_updates), 0},
                              {reinterpret_cast<void*>(&tab_about), 0}, {reinterpret_cast<void*>(&tab_get), 0},
                              {reinterpret_cast<void*>(&tab_all), 0}};
    static const char* const names[] = {"PART_Button_ModsInstalled", "PART_Button_ModsUpdates", "PART_Button_ModsAbout",
                                        "PART_Button_ModsGet", "PART_Button_ModsAll"};
    for (int i = 0; i < 5; ++i) reinterpret_cast<OnClick>(base + RVA_ON_CLICK)(page, names[i], &pmf[i]);
}

} // namespace

void start_mods_page() {
    // The game isn't mapped yet (Azimuth maps it after the platform loads); hook_game_function waits for it.
    std::thread([] {
        hook_game_function(RVA_SET_STATE_A, reinterpret_cast<void*>(&set_state_a), "the tile state setter", reinterpret_cast<void**>(&g_set_a));
        hook_game_function(RVA_SET_STATE_B, reinterpret_cast<void*>(&set_state_b), "the tile lock setter", reinterpret_cast<void**>(&g_set_b));
        hook_game_function(RVA_SAVE, reinterpret_cast<void*>(&save_detour), "the game's save", reinterpret_cast<void**>(&g_save));
        hook_game_function(RVA_WIRE, reinterpret_cast<void*>(&wire), "the Home tab's tile wiring", reinterpret_cast<void**>(&g_wire));
        hook_game_function(RVA_HANDLER_A, reinterpret_cast<void*>(&handler_a), "the Horizon Group tile's click handler");
        hook_game_function(RVA_HANDLER_B, reinterpret_cast<void*>(&handler_b), "the Horizon Group tile's second handler");
    }).detach();
}

} // namespace fh3platform
