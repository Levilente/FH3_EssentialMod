// HelloMod: the smallest useful FH3 mod, showing the platform API.
//
// Build it (this repo's CMake builds it as HelloMod.dll), drop it in
// fh3-mods next to azimuth.exe, and launch the game:
//   - fh3-mods\platform.log gets a few lines from it
//   - Home > MODS > Installed mods > HelloMod has an On/Off row
//   - once the game is mapped, it logs where
#include "fh3mod.h"

#include <windows.h>

#include <cstdio>

namespace {

const FH3ModApi* g_api = nullptr;
FH3Mod* g_mod          = nullptr;

DWORD WINAPI wait_for_game(LPVOID) {
    // Anything slow goes on your own thread, never in fh3mod_init.
    if (void* base = g_api->wait_for_game(0xFFFFFFFFu)) {
        char line[96];
        std::snprintf(line, sizeof line, "the game is mapped at %p", base);
        g_api->log(g_mod, line);
    }
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

    api->log(g_mod, "hello from HelloMod");

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

    if (HANDLE t = CreateThread(nullptr, 0, wait_for_game, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}
