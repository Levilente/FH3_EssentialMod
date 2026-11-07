// HelloMod: the smallest useful FH3 mod, showing the platform API.
//
// Build it (this repo's CMake builds it as HelloMod.dll), drop it in
// fh3-mods next to azimuth.exe, and launch the game:
//   - fh3-mods\platform.log gets a few lines from it
//   - Home > MODS > Installed mods > HelloMod has an On/Off row
//   - once the game is mapped, it logs where
//   - with fh3-mods\platform.ini [dev] HotReload=1 you can rebuild it while the game runs and press F9: fh3mod_shutdown stops its
//     thread, and the new build starts in its place (see docs/writing-a-mod.md, Hot reload)
#include "fh3mod.h"

#include <windows.h>

#include <cstdio>

namespace {

const FH3ModApi* g_api = nullptr;
FH3Mod* g_mod          = nullptr;

HANDLE g_stop   = nullptr;   // set by fh3mod_shutdown: the thread's reason to end
HANDLE g_thread = nullptr;

DWORD WINAPI wait_for_game(LPVOID) {
    // Anything slow goes on your own thread, never in fh3mod_init. A thread that waits for something must wait for g_stop too,
    // so fh3mod_shutdown can end it.
    if (void* base = g_api->wait_for_game(2000)) {
        char line[96];
        std::snprintf(line, sizeof line, "the game is mapped at %p", base);
        g_api->log(g_mod, line);
    }
    WaitForSingleObject(g_stop, 1000);
    return 0;
}

} // namespace

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api) {
    if (api->api_version < 1) return 1;
    g_api = api;

    FH3ModInfo info{};
    info.size             = sizeof info;
    info.name             = "HelloMod";
    info.version          = "1.0.0";
    info.github_repo      = nullptr; // "You/YourRepo" to get update notices
    info.platform_version = FH3MOD_PLATFORM_VERSION;
    info.check_updates    = 0;
    g_mod                 = api->register_mod(&info);
    if (!g_mod) return 1;

    api->log(g_mod, api->api_version >= 16 && api->is_reloading() ? "hello again, from a hot reload" : "hello from HelloMod");

    // A row on the MODS page (API 4): the mod keeps the value, the platform
    // shows it and calls on_change when the player flips it.
    if (api->api_version >= 4) {
        FH3Setting row{};
        row.size        = sizeof row;
        row.key         = "Greeting";
        row.label       = "Say hello";
        row.description = "Writes a line to platform.log when you switch it.";
        row.value       = 1;
        row.on_change   = [](void*, const char*, int value) { g_api->log(g_mod, value ? "hello again" : "quiet now"); };
        api->add_setting(g_mod, &row);
    }

    g_stop   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, wait_for_game, nullptr, 0, nullptr);
    return 0;
}

// Optional (API 16): lets the platform swap this DLL for a rebuilt one while the game runs. Stop everything the mod started and wait
// for it to end; the platform switches the hooks off afterwards and calls the new build's fh3mod_init.
FH3MOD_EXPORT void fh3mod_shutdown(void) {
    if (g_stop) SetEvent(g_stop);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stop) {
        CloseHandle(g_stop);
        g_stop = nullptr;
    }
}
