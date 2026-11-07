# Writing a mod

This walks through making a Forza Horizon 3 mod on the platform, from an empty folder to a release players can install. The example is
[`examples/hello_mod`](../examples/hello_mod/hello_mod.cpp); copying it is a fine way to start. Every call is described in [api.md](api.md),
and a mod that changes the player's car should also read [car-mods.md](car-mods.md).

Complete mods built on the platform, worth reading:

| Mod | Shows how to |
|---|---|
| [NitroSystem II](https://github.com/Levilente/NitroSystem-II---FH3) | an upgrade with an effect while driving, hooks on the car and the camera, HUD changes through `set_file`, many MODS page rows |
| [Universal Wheel Spacers](https://github.com/Levilente/UniversalWheelSpacers_FH3) | two upgrades, one instruction hook in the car loader, `upgrade_stage_for_car`, custom icons |
| [Airride](https://github.com/Levilente/AirRide_FH3) | an upgrade with a tuning row (`add_tuning_row`), a shop preview of its own (`upgrade_preview_stage`), controller input |
| [Launch Control](https://github.com/Levilente/FH3_LaunchControl) | an upgrade with a tuning row, hooks into the car's physics step, controller input, **`fh3mod_shutdown`** (hot reload) |
| [DualSense](https://github.com/Levilente/FH3_DualSense) | a controller driver as a mod: HID output, HD haptics on an audio endpoint, hooks that read the game's own car feedback, game textures changed in memory; **`fh3mod_shutdown`** with several threads |
| [Drag and Drift Tires](https://github.com/Levilente/FH3_Drag-DriftTires), [Livery Decals](https://github.com/Levilente/LiveryMod_FH3) | mods that are only data: `add_sql`, `add_string_key`, `set_file` |

## What you need

A mod is a 64-bit Windows DLL that exports one C function. C++ is what the existing mods use, and the instructions here assume it.

- **To build:** [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) and CMake (Linux or Windows; every mod here uses it), or Visual Studio
  with CMake. The game's memory is read with `__try`/`__except`, so llvm-mingw needs `-fms-extensions`; GCC-based MinGW won't do.
- **To test:** Forza Horizon 3 (the Xbox One build, 1.0.125.0) through Azimuth, with the platform installed (`DWrite.dll` and `d3dcompiler_47.dll`
  next to `azimuth.exe`). File, text and database changes go to the game's `mediaoverride` folder, which Extra Options makes the game read; without
  Extra Options the platform does it itself.

## Set up the project

Add the platform to your repo, so your build has the header to compile against:

```bash
git submodule add https://github.com/Levilente/FH3_ModPlatform platform
```

The header is all a mod needs (`platform/include/fh3mod.h`). The mods here use this `CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.20)
project(mymod VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(FH3_PLATFORM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/platform" CACHE PATH "FH3 Mod Platform checkout")
file(STRINGS "${FH3_PLATFORM_DIR}/CMakeLists.txt" _pv REGEX "^project\\(fh3_mod_platform VERSION")
string(REGEX REPLACE ".*VERSION ([0-9.]+).*" "\\1" FH3_PLATFORM_VERSION "${_pv}")

add_library(mymod SHARED src/mymod.cpp)
target_include_directories(mymod PRIVATE "${FH3_PLATFORM_DIR}/include")
target_compile_definitions(mymod PRIVATE MYMOD_VERSION="${PROJECT_VERSION}" FH3MOD_PLATFORM_VERSION="${FH3_PLATFORM_VERSION}"
                                         UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(NOT MSVC)
    target_compile_options(mymod PRIVATE -fms-extensions)   # __try/__except
    target_link_options(mymod PRIVATE -static -s)           # no libc++/libunwind DLLs to ship
endif()
set_target_properties(mymod PROPERTIES OUTPUT_NAME MyMod PREFIX "")
```

Build with the platform's toolchain file:

```bash
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=platform/cmake/mingw-w64-x86_64.cmake
cmake --build build
```

(`add_subdirectory(platform)` and linking `fh3mod_api` works too; it builds the platform alongside.)

## The entry point

Export `fh3mod_init`. The platform calls it once, after loading your DLL, on its own thread and never under the Windows loader lock, so you can load
DLLs, create files and start threads from it, which you can't safely do from `DllMain`.

```cpp
#include "fh3mod.h"
#include <windows.h>

static const FH3ModApi* g_api;
static FH3Mod* g_mod;

static DWORD WINAPI start(LPVOID) {
    char* game = static_cast<char*>(g_api->wait_for_code(0xFFFFFFFFu));
    if (!game) return 0;   // not FH3, or another build
    // ... hooks go here ...
    return 0;
}

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api) {
    if (api->api_version < 11) return 1;   // the oldest platform you support
    g_api = api;
    FH3ModInfo info{};
    info.size             = sizeof info;
    info.name             = "MyMod";
    info.version          = MYMOD_VERSION;
    info.github_repo      = "You/MyMod";      // update checks; nullptr for none
    info.platform_version = FH3MOD_PLATFORM_VERSION;
    info.check_updates    = 1;
    info.description      = "One or two sentences for MODS > Installed mods.";
    g_mod = api->register_mod(&info);
    if (!g_mod) return 1;
    // ... rows, upgrades, files, SQL ...
    if (HANDLE t = CreateThread(nullptr, 0, start, nullptr, 0, nullptr)) CloseHandle(t);
    return 0;
}
```

Keep `fh3mod_init` quick: mods start one after another, and the platform writes the game's files once all of them have returned, before the game
reads them. Anything slow (waiting for the game, network, big files) goes on a thread you start yourself. Settings rows, upgrades, tuning rows,
SQL and `set_file` belong in `fh3mod_init`: they are written into the game's data before it loads.

Check `api->api_version` before using anything newer than what you require: members are only ever added at the end of the API table.

## What a mod can do

The table at the top of [api.md](api.md#everything-you-can-use) lists every call. In short:

- **Settings in the game's menus.** On/Off rows, sliders, named choices and links for your mod on **Home > MODS > Installed mods** (API 4 to 9).
  You keep the value (in your ini) and get told when the player changes it. Rows that need a restart are marked, and the game offers to restart.
- **Garage upgrades.** `register_upgrade` (API 11) gives you a real upgrade under **Garage > Upgrades**, with stages, prices and icons; the platform
  runs the shop, basket and purchase and keeps each car's stage. Your mod adds the effect. See [car-mods.md](car-mods.md).
- **Tuning rows.** `add_tuning_row` (API 13) puts a slider of yours on the car's **Tuning > Springs** page.
- **Game files.** `set_file` replaces a file the game reads from `media` (a texture, a XAML page, an XML file).
- **The game database.** `add_sql` changes `gamedbRC.slt` (cars, prices, events, upgrades) with plain SQL.
- **Texts.** `add_string_key` adds new texts to FH3's string tables. Changing the game's existing texts isn't available for FH3 yet.
- **Game code.** Hooks on functions, calls, pointers and single instructions, and byte patches (API 3, 7), shared safely with other mods.
- **Hotkeys and commands.** A key or controller chord that calls your code, and commands you can send from outside the game with `fh3mod.exe`.
- **Debugging help.** Sessions, a log per session, save snapshots before a risky test, crash reports that name your mod and include your memory.

## Working with the game

Azimuth maps the game, `forza_durango_release_final.exe`, into its own process when it starts it, so it isn't there yet when your mod loads.
`wait_for_code(timeout)` waits until it is, and until Extra Options has finished patching, and returns its base address. Use it (not `wait_for_game`)
before hooking game code, from your own thread. It returns null when the game started isn't this FH3 build: stop there.

Only one build is in circulation: the Xbox One release of 5 June 2018 (1.0.125.0). Its addresses are fixed, so mods use offsets from the base
(`game+0x...`). Even so, check that the bytes you're about to hook look the way you expect, and log and skip if they don't; a wrong guess is then a
missing feature, not a crash. The platform also compares every hook with the exe on disk and refuses one on code someone else already changed.

Where to find addresses: [`include/fh3game.h`](../include/fh3game.h) names vtables and places with their bytes for your check;
[`fh3-game-notes.md`](fh3-game-notes.md) has what was found while building the platform and the mods (the car objects and their offsets, the car
loader, the upgrade shop, the menus); the mods' sources say what each of their addresses is.

Game code runs on many threads, so your detour can be called from any of them as soon as the hook is on. Set up everything it needs first, then call
`enable_hooks`, and keep detours short: some run every frame for every car.

## A mod that changes the player's car

Read [car-mods.md](car-mods.md). The short version: register an upgrade, and

- for an effect while driving, use the stage the platform reports for the driven car (`on_stage`, or `upgrade_stage(mod, handle, 0)`): 0 in cars that
  aren't the player's;
- for a change to how the car is built, hook the car loader and ask `upgrade_stage_for_car(mod, handle, car_id)` from inside the hook: 0 for AI
  cars of the same model and loaners, the previewed stage in the shop.

Don't track the driven car yourself and don't compare car models; the platform does that for every mod. Never write your part into the game's part
set or tuning (enums 0 to 42 are the game's); the platform keeps bridge parts out of it.

## Hot reload: change your mod without restarting the game

Starting FH3 takes minutes; with hot reload a rebuilt mod is running a few seconds after you press a key. Turn it on in `fh3-mods\platform.ini` (the platform writes the keys on its first start):

```ini
[dev]
HotReload=1
ReloadKey=F9
```

Then the loop is: edit, build (the linker can now overwrite `fh3-mods\YourMod.dll` while the game runs, because the platform loaded a copy), click into the game, press **F9**. `platform.log` says what happened (`hot reload: shutting down 1.0.0` ... `hot reload: 1.0.1 is running`), and the platform beeps (two rising tones: swapped; a low one: something failed; a short one: no DLL has changed).

**What your mod has to do** is export `fh3mod_shutdown`, next to `fh3mod_init`:

```cpp
static std::atomic<bool> g_stop{false};
static HANDLE g_thread = nullptr;

static DWORD WINAPI worker(LPVOID) {
    while (!g_stop.load()) {
        // ... the work ...
        Sleep(8);                      // a long wait has to be interruptible: WaitForSingleObject(g_stop_event, ms)
    }
    return 0;
}

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api) {
    // ... register_mod, rows, upgrades, hooks as always ...
    g_thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
    return 0;
}

FH3MOD_EXPORT void fh3mod_shutdown(void) {
    g_stop.store(true);
    if (g_thread) { WaitForSingleObject(g_thread, 3000); CloseHandle(g_thread); g_thread = nullptr; }
    // undo what you changed yourself: memory you wrote in the game, your own patches, files you opened
}
```

The platform then switches your hooks off itself, loads the new build and calls its `fh3mod_init`. The rules for a mod that wants to be reloadable:

- **Stop every thread you started, and wait for it** (with a timeout of a few seconds), including threads that wait for the game (`wait_for_code`, `wait_for_game` can be given a timeout) and ones that sleep for long (wait on a stop event, not `Sleep(25000)`). You have 8 seconds in all.
- **Undo what the platform doesn't know about.** The platform removes what went through its API (hooks, patches made with `patch_bytes`, rows' callbacks). Memory you wrote in the game yourself, a vectored exception handler, a window, a device handle, a changed texture: put it back in `fh3mod_shutdown`.
- **Keep the state in your DLL's globals.** The new build is a new copy of the DLL with fresh globals; it reads its settings from your ini again. Nothing needs carrying over, and nothing can: the old build's data stays where it was.
- **Do everything that depends on the game's data in `fh3mod_init` as usual.** Texts, files, SQL, new rows and an upgrade's stages are read by the game at startup and are not applied again on a reload (see the table in [api.md](api.md#hot-reload-api-16)): a change to one of those shows after a restart. The rest of your mod (your logic, your hooks and callbacks, the numbers you compute, the effects) is what hot reload is for.
- **`is_reloading()`** (API 16) says the call to `fh3mod_init` is a reload, for the rare mod that needs to behave differently.
- `fh3mod_shutdown` runs on a thread of the platform's, not a game thread, so it can't do anything that needs the game's own thread. Don't call `disable_hooks` in it: the platform does that right after it returns.

A mod without `fh3mod_shutdown` is left running; the log says the new build needs a restart. Mods that only add data (SQL, files, texts) need no `fh3mod_shutdown`: the game reads that at startup and a restart is needed anyway.

**It is a developer feature, and off by default** (`HotReload=0`): players never load a copy and never press the key. With it on, each reload adds the DLL's size to the game's memory (the old copy is never unloaded, so nothing can fall into unmapped code), and a bug in a `fh3mod_shutdown` can still hang or crash the game: it is code you wrote running inside the game. Don't rely on it to test a release; do that with a plain start.

## Your own files

Keep your settings in `fh3-mods\<YourMod>.ini` (the existing mods do) or in a folder of your own next to `azimuth.exe`; `api->azimuth_dir` gives
you that folder and `api->game_dir` the game's `Mount` folder. Don't put other `.dll` files in `fh3-mods`: everything there ending in `.dll` is loaded
as a mod. Write your ini with its defaults on the first start, so players can find the keys.

**Logging.** Keep `platform.log` short by default: problems always, details only behind a `Log=1` (or `DebugLog=1`) switch in your ini. The
platform does the same with `fh3-mods\platform.ini` `[log] Verbose`. Players send the log with bug reports; a short one is read.

## Icons

Upgrade icons are FH3 `.swatchbin` textures. The mods here keep 200 x 200 PNGs in `icons/`, convert them with `scripts/gen_icons.py` (pillow,
numpy) into a header that's packed into the DLL, and hand them to the game with `set_file` as
`ui/textures/data_bound/Upgrade_Parts/Custom/<name>_Icon.swatchbin`. Copy the script and `scripts/icontools/` from any of them.

## Shipping it

The release is your DLL, nothing else: players copy it into `fh3-mods`. The platform comes from its own
[releases page](https://github.com/Levilente/FH3_ModPlatform/releases/latest), once for every mod. Say in your README which platform version you
need (`FH3 Mod Platform 1.0.0 or newer`) and what your ini keys do.

- Tag the release `vX.Y.Z` with the same version your mod registers: the platform's update check compares the two (a leading `v` is ignored).
- Before releasing, start the game **without** hot reload and with only your mod and check `fh3-mods\platform.log` (with your log switch on): your DLL, its version, your
  hooks and changes, no problems. Then try it next to the other mods.
- A repo that is private can't be checked for updates, and players can't download from it.

## Etiquette

- **Don't proxy DLLs yourself.** `DWrite.dll` and `d3dcompiler_47.dll` are the platform's, `winmm.dll` is Extra Options'.
- **Hook through the platform.** Your own MinHook or byte patches can't see other mods' hooks, and the platform can't warn about clashes.
- **Don't write into the game's own files.** Use `set_file`, `add_sql` and `add_string_key`, so removing your mod puts everything back.
- **Make features switchable**, and undo what a switched-off feature did.
- **Don't take ages to start.** Do your setup on your own threads.
- **Mind other players' cars.** Upgrades belong on the player's own cars; let the platform tell you which those are.
