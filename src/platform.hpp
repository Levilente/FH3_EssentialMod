// Shared between the platform's source files.
#pragma once

#include "fh3mod.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

struct FH3Mod {
    std::string name, version, repo, platform_version, description;
    std::wstring wname, dll;
    bool check_updates = true;
    bool offline_only  = false;      // FH3ModInfo.offline_only (API 9)
    // table (lower case) -> edits
    std::map<std::string, std::map<uint16_t, std::wstring>> set;
    std::map<std::string, std::map<std::wstring, std::wstring>> repl;
    // table (lower case) -> hashed-key strings to append (API 10, add_string_key)
    std::map<std::string, std::map<uint32_t, std::wstring>> add;
    std::vector<std::string> notice; // from set_notice
    std::vector<std::string> status; // the platform's own: update available, platform too old
    std::set<std::wstring> files;    // mediaoverride paths this mod owns (lower case)
    std::vector<std::string> sql;    // database statements, in the order given
    std::string latest;              // latest release on GitHub, once checked
    bool reloaded = false;           // hot-reloaded at least once (API 16): add_sql ignores statements it has
};

namespace fh3platform {

// One line to platform.log; `who` is a mod name, or "" for the platform.
void log_line(const std::string& who, const std::string& text);
inline void plog(const std::string& text) { log_line("", text); }
// The platform's detailed lines (shop steps, garage reads, car builds, units): only with fh3-mods\platform.ini
// [log] Verbose=1 (off by default; for bug reports).
bool log_verbose();
inline void vlog_line(const std::string& who, const std::string& text) { if (log_verbose()) log_line(who, text); }
inline void vplog(const std::string& text) { if (log_verbose()) log_line("", text); }

// Hot reload (platform.cpp, API 16): true while `mod`'s fh3mod_init runs again after a reload. A registration with a key
// that exists already then swaps the entry's callbacks instead of failing or adding it twice.
bool reload_rebinding(const FH3Mod* mod);
// Blocks until every fh3mod_init has returned (or the time is up). For the platform's own threads.
bool wait_mods_ready(uint32_t timeout_ms);

// hooks.cpp: hooks a function of the game for the platform itself (blocks until the game's code is ready, so call
// it from its own thread). Returns the trampoline to the original, or nullptr.
void* hook_game_function(uint32_t rva, void* detour, const char* what, void** original = nullptr);
// The same in the hook chain the mods use, for a function mods may hook too.
void* hook_game_function_shared(uint32_t rva, void* detour, const char* what, void** original);
// upgrades.cpp: the upgrade bridge (API 11).
int upgrades_register(FH3Mod* mod, const FH3UpgradeDef* def, const std::function<void(const char*, uint32_t, const wchar_t*)>& add_key,
                      const std::function<void(const std::string&)>& add_sql);
int upgrades_stage(int handle, int32_t garage);
uint32_t upgrades_active_garage();
int32_t upgrades_car_model(int32_t garage);
int32_t upgrades_garage_for_car(int32_t car_id, uint32_t flags);
int upgrades_stage_for_car(int handle, int32_t car_id);
int upgrades_preview_stage(int handle);
uint32_t upgrades_shop_serial();
bool show_message_popup(void* page, const std::wstring& title, const std::wstring& message);   // the game's message box with an OK item
void upgrades_import(int handle, int32_t garage, int32_t ordinal, int stage, uint32_t owned_mask);
void upgrades_start_hooks();
void* upgrades_live_db();
// sellcar.cpp: SELL CAR in the garage's car menu.
void sellcar_start();
// intro_skip.cpp: the start-up video is skipped.
void start_intro_skip();
void start_input(); // input.cpp: other controllers (DualSense, ...) for the mods' XInput reads
// mods_page.cpp: takes over the MODS tile on the Home tab.
void start_mods_page();

// Base of the mapped game exe, or nullptr (always, once Azimuth has started another game).
void* find_game();
constexpr uint32_t kGameSizeOfImage = 0x05495000u; // FH3's forza_durango_release_final.exe, 5 Jun 2018 (TimeDateStamp 0x5B16ECCA)
// The parts of the platform that rely on addresses in the game's code or on the game's data layout (the
// MODS tab and Settings page, title-screen entries, the mediaoverride mount, game text, files and database
// changes) were found in FH2 and are switched off here until they are found again in FH3. Hooks, logging,
// crash reports, hotkeys, commands and the mods' own services still work.
constexpr bool kGameFeatures = false;
// The Mods page (settings.cpp): mods' rows on pages laid out like the HUD options page, opened from the Home
// tab's MODS tile. The FH3 addresses are in docs/mods-page-fh3-map.md.
constexpr bool kModsPage = true;
// Game database patches (database.cpp): gamedbRC.slt is plain SQLite on FH3, so mods' SQL works.
constexpr bool kDatabase = true;
// The mediaoverride and mediapc folders mounted over game:\media\ (media_overrides.cpp), ported to FH3.
constexpr bool kMediaOverrides = true;
// Copies n bytes, false if any of them can't be read or written; dst may then hold part of the copy,
// so don't use it. For memory that can change under the platform: the game's image while Azimuth
// maps it, or a region freed after VirtualQuery.
bool safe_copy(void* dst, const void* src, std::size_t n);

// titles.cpp: which game Azimuth started. Azimuth 1.0.1 runs several Forza games, all from a
// forza_durango_release_final.exe mapped at 0x140000000 (platform 2.8.0).
enum class Launched { not_yet, home, other };
Launched launched_game();
const char* launched_game_name(); // the other game, for a log line or crash report ("" until then)
// A game other than FH2 that Azimuth's command line starts straight away (--skip-launcher, or
// --game=DIR), with how it was told, for the log; "" for FH2, or when the launcher shows first.
// When it's another game, launched_game() says so from then on.
std::string other_game_on_command_line();
std::filesystem::path command_line_game_folder(); // --game=DIR, else empty
bool holds_home_exe(const std::filesystem::path& folder); // in its Mount folder, or itself
// The other game's name when `p` is in a game exe mapped at 0x140000000 that isn't FH2's
// 24 Nov 2015 one, else "". Nothing is hooked or patched there.
std::string other_game_at(const void* p);
// Watches for Azimuth mapping the game, says in platform.log which one it is, and then calls
// `home` or `other` on its own thread.
void watch_launched_game(std::function<void()> home, std::function<void()> other);

// The id of the [title-<id>] in data\azimuth.ini whose exe on disk is the game this platform is made for
// (matched by the exe's header, so no title id is hard-coded), "" if none is. Its folder, absolute.
std::wstring home_title_id();
std::filesystem::path home_title_folder();

std::filesystem::path azimuth_dir();
std::filesystem::path game_mount(); // empty if not found

// units.cpp: speed and power units chosen on their own (0 = the game's own, then the units).
int units_get(int which);
void units_set(int which, int v);
void units_start();

// files.cpp: file overrides in Extra Options' mediaoverride folder.
int set_file(FH3Mod* mod, const char* media_path, const void* data, std::size_t size);
void clear_file(FH3Mod* mod, const char* media_path);
// Removes overrides from earlier launches that no mod owns now.
void clean_leftover_files(const std::vector<FH3Mod*>& mods);
// Removes one override from an earlier launch now, if no mod replaces it this time.
void remove_leftover_file(const char* media_path);

// media_overrides.cpp: mounts mediaoverride and mediapc itself when Extra Options isn't loaded.
void start_media_overrides();
// ui_patches.cpp: writes the platform's own overlay folder (patched copies of game UI files); empty if nothing to mount.
std::filesystem::path build_ui_overlay();
// The Mods page (the placeholder page in the overlay): header and rows as XAML fragments.
void write_mods_page(const std::string& header, const std::string& rows);
inline constexpr const wchar_t* kOverlayFolder = L"mediaplatform"; // under the game folder; mounted as game:\mediaplatform\
// With Extra Options the platform cannot mount its own folder (EO refuses its patches if the file lookup was changed
// first), so the menu patches go into EO's mediaoverride instead (same layout: UI\..., stateflow\..., Stripped\...).
const wchar_t* overlay_folder();

// database.cpp: SQL patches applied to a copy of the game database.
void apply_database(const std::vector<FH3Mod*>& mods);

// hooks.cpp: the hook API, and what crash reports say about hooks.
// The folder of Extra Options' winmm.dll if that's the winmm.dll loaded, else empty. Extra
// Options is recognised by its Fh2TweaksStatus export, not by where winmm.dll is: Wine and
// Proton load their own winmm.dll instead of the one beside azimuth.exe unless told otherwise
// (WINEDLLOVERRIDES="winmm=n,b"), and still report that file's path for it.
std::filesystem::path extra_options_dir();
void fill_hook_api(FH3ModApi& api);
bool hook_at(const void* p, char* out, std::size_t n); // p is in a hook's trampoline or stub
int hook_count();
const char* hook_line(int i, bool* enabled);

// settings.cpp: rows on the game's Mods page (API 4).
int settings_add(FH3Mod* mod, const FH3Setting* setting);
// Slider rows on the garage's tuning menu (API 13).
int tuning_row_add(FH3Mod* mod, const FH3TuningRow* row);
int settings_get(FH3Mod* mod, const char* key);
void settings_set(FH3Mod* mod, const char* key, int value);
float settings_get_slider(FH3Mod* mod, const char* key);
void settings_set_slider(FH3Mod* mod, const char* key, float value);
// Pages of the mods' own (API 8).
int pages_add(FH3Mod* mod, const FH3Page* page);
int pages_set_rows(FH3Mod* mod, const char* page_key, const FH3PageRow* rows, uint32_t count);
int pages_set_xaml(FH3Mod* mod, const char* page_key, const char* resources, const char* content, const char* help);
int pages_get_slot(FH3Mod* mod, const char* page_key, int slot);
int pages_set_slot(FH3Mod* mod, const char* page_key, int slot, int value);
int pages_focus(FH3Mod* mod, const char* page_key, const char* name);
int pages_set_tile_text(FH3Mod* mod, const char* page_key, const char* text);
int pages_set_tile_colors(FH3Mod* mod, const char* page_key, uint32_t background, uint32_t text);
// Title-screen entries (API 9).
int title_add(FH3Mod* mod, const FH3TitleEntry* entry);
const char* title_started_by(FH3Mod* mod);
// After every fh3mod_init: writes the pages and hooks the game if any mod added a row.
bool settings_hub_open(int which);
void mods_reopen(int which);
void settings_start(const FH3ModApi* api);

// The MODS tab (platform 2.5.0): every mod in fh3-mods, loaded or turned off there.
struct ModEntry {
    std::wstring dll;                 // file name in fh3-mods
    std::string name, version, description, repo;
    bool enabled = true;              // loaded this time (false: turned off on the MODS tab)
    std::string problem;              // why it didn't load or start ("" if it did)
    bool platform_mod = false;        // has fh3mod_init (an old-style mod doesn't register)
    FH3Mod* mod = nullptr;            // when loaded and registered
    bool offline_only = false;        // its author says it shouldn't run online (FH3ModInfo.offline_only)
    bool loads_online = true;         // loaded when playing online (the player's choice, else !offline_only)
    bool online_off = false;          // not running this time because the game is online (FH2MP)
};
std::vector<ModEntry> mod_entries();  // in load order, turned-off ones included
// fh3-mods\platform-disabled.txt: the DLLs the MODS tab turned off (lower case).
std::set<std::wstring> disabled_mods();
void set_mod_enabled(const std::wstring& dll, bool on);
// Online play (platform 2.8.0): the game was started from the FH2MP launcher. Decided once,
// before the mods load.
bool playing_online();
bool fh2mp_installed();               // FH2MP.dll is in fh3-mods (turned on or not)
// fh3-mods\platform-online.txt: the player's choice per DLL, "Load when playing online".
void set_mod_online(const std::wstring& dll, bool on);
// The latest release of a mod on GitHub, once checked ("" before that, or if unknown).
std::string latest_known(const FH3Mod* m);

// session.cpp: this run of the game (API 9). start_session makes the id and opens the copy of
// platform.log that fh3-mods\platform-logs keeps; session_line is platform.log's line about it.
FILE* start_session();
std::string session_line();
const char* session_id();
const wchar_t* session_log_path(FH3Mod* mod, const wchar_t* folder, uint32_t keep);
// fh3-mods\platform-session.json: rewritten as the mods load and the command pipe starts, and
// marked crashed (with the report's name, relative to fh3-mods) by the crash handler.
void write_session_status();
void set_session_pipe(const std::string& name);
void write_session_crashed(const wchar_t* fh3mods_dir, const char* report);
// Copies of the saves folder that mods ask for from fh3mod_init (API 9), made once they've all
// returned (start_save_snapshot).
int request_save_snapshot(FH3Mod* mod, const char* reason, uint32_t keep);
void start_save_snapshot();

// crash.cpp
struct LoadedDll {
    std::wstring name;
    const char* base;
    std::size_t size;
};
void install_crash_handler(const std::vector<LoadedDll>& mods);
void add_crash_dll(const LoadedDll& dll); // one loaded later (a mod from before the platform)
void recheck_crash_handler();
void add_dump_memory(FH3Mod* mod, const void* p, std::size_t n); // memory a mod wants in every dump (API 9)

// hotkeys.cpp: keys and controller chords that call mods back, with rows on the Mods page (API 9).
int hotkey_add(FH3Mod* mod, const FH3Hotkey* hotkey);

// commands.cpp: mods' commands, run through a named pipe (API 9).
int command_add(FH3Mod* mod, const char* name, const char* help, FH3CommandFn fn, void* user);

} // namespace fh3platform
