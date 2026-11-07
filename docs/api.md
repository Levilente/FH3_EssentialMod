# API reference

Everything is in [`include/fh3mod.h`](../include/fh3mod.h). This page goes through it member by member. The FH3 Mod Platform 1.0.0 has **API version 15**.

The platform is a fork of the FH2 Mod Platform, and the API keeps its numbering, so a mod checks `api->api_version` the same way on both. A few parts were written for FH2's menus and files and do nothing in FH3 yet; the table says which.

## Everything you can use

| What | Calls | API | In FH3 |
|---|---|---|---|
| Register, log, version, folders | `register_mod`, `log`, `mod_version`, `azimuth_dir`, `game_dir` | 1 | yes |
| Find the game, wait for its code | `game_base`, `wait_for_game`, `wait_for_code` | 1, 3 | yes |
| Hooks on functions, calls, pointers | `hook_function`, `hook_call`, `hook_pointer`, `enable_hooks`, `disable_hooks` | 3 | yes |
| Byte patches, hooks on one instruction | `patch_bytes`, `hook_instruction` | 7 | yes |
| Replace a game file (`media`) | `set_file`, `clear_file` | 2 | yes |
| Change the game database (SQL) | `add_sql`, `clear_sql`, `apply_database` | 2 | yes |
| New texts in FH3's string tables | `add_string_key` | 10 | yes |
| Change existing texts | `set_string`, `replace_string`, `clear_strings`, `apply_strings` | 1 | no: FH3's tables use 32-bit hashed keys, these work on FH2's |
| Title-screen notices | `set_notice` | 1 | no (stored, not shown); update checks show on MODS > Updates |
| Settings rows on the MODS page (On/Off, sliders, links, choices) | `add_setting`, `get_setting`, `set_setting`, `get_slider`, `set_slider` | 4, 5, 6, 9 | yes |
| Hotkeys (keyboard and controller) | `add_hotkey` | 9 | yes |
| Commands from outside the game (`fh3mod.exe`) | `add_command` | 9 | yes |
| Interfaces between mods | `publish_interface`, `get_interface`, `wait_for_mods` | 2 | yes |
| Sessions, own log files, save snapshots, crash dump memory | `session_id`, `session_log_path`, `request_save_snapshot`, `add_dump_memory` | 9 | yes |
| Garage upgrades (shop, basket, purchase, per-car stages) | `register_upgrade`, `upgrade_stage`, `upgrade_active_garage`, `upgrade_import` | 11 | yes |
| Whose car is it (the player's, a loaner, an AI car) | `upgrade_car_model`, `upgrade_garage_for_car`, `upgrade_stage_for_car` | 12, 14 | yes |
| Shop preview for effects outside the car's data | `upgrade_preview_stage`, `upgrade_shop_serial` | 15 | yes |
| Slider rows on the car's Tuning > Springs page | `add_tuning_row` | 13 | yes |
| Pages of your own, tiles | `add_page`, `set_page_rows`, `set_page_xaml`, `set_page_tile_text`, ... | 8 | no: return 0 (FH2 menus only) |
| Title-screen entries | `add_title_entry`, `title_started_by` | 9 | no: return 0 (FH2 menus only) |
| Online play | `playing_online`, `offline_only` | 9 | no online play for FH3: `playing_online` is always 0 |

Where to start: [writing-a-mod.md](writing-a-mod.md) (an empty folder to a release), [car-mods.md](car-mods.md) (mods that change the player's car), [fh3-game-notes.md](fh3-game-notes.md) (what is known about the game's code: addresses, objects, menus).

## Rules that apply everywhere

- **Threads:** every function is thread-safe and can be called from any thread.
- **Strings:** strings passed in are copied, so you can free yours straight after. Names, versions, log lines and notices are UTF-8 (`const char*`). Game text is UTF-16 (`const wchar_t*`), because that's what the game uses.
- **Memory:** nothing the platform returns needs freeing, and the `FH3ModApi` pointer stays valid until the process ends.
- **Compatibility:** members are only ever added at the end of `FH3ModApi`. If you use one added after version 1, check `api->api_version` (or `api->size`) first, so your mod still runs on an older platform.

## Entry point

```c
FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api);
```

Export this from your DLL. The platform loads every DLL in `fh3-mods` in alphabetical order, then calls each one's `fh3mod_init` in the same order, on its own thread and outside the loader lock. Return 0 on success. Any other value, or a crash inside the call, is logged; the crash is caught, so it won't take the game down. Once every mod's `fh3mod_init` has returned, the platform writes all string-table edits and notices.

DLLs without `fh3mod_init` still load, and are logged as mods from before the platform. They load once the game Azimuth starts is known to be FH3, and not at all for another game ([game_base](#game_base-and-wait_for_game)), since they change the game by themselves.

## FH3ModApi fields

| Field | Meaning |
|---|---|
| `size` | `sizeof(FH3ModApi)` as the platform knows it. |
| `api_version` | The platform's `FH3MOD_API_VERSION`. |
| `platform_version` | The platform's version, like `"1.0.0"`. |
| `azimuth_dir` | The folder with `azimuth.exe`, without a trailing slash. Put your data folder here. |
| `game_dir` | The game's `Mount` folder (`game:\`), or `""` if it wasn't found. |

## register_mod

```c
FH3Mod* register_mod(const FH3ModInfo* info);
```

Call once, from `fh3mod_init`. It returns your handle, or null if `info` is missing or too small. Fill in every member of `FH3ModInfo`:

| Member | Meaning |
|---|---|
| `size` | `sizeof(FH3ModInfo)` |
| `name` | Display name, used in the log and in notices. |
| `version` | Your version, `x.y.z`. |
| `github_repo` | `"Owner/Repo"`, or null. When set (and `check_updates` is 1), the platform checks your latest release on GitHub, at most every six hours, and shows it on **MODS > Updates** when it's newer than `version`. The six hours start over when a different `version` is installed, and a remembered release older than `version` is never trusted, so installing an older or newer build checks again straight away. |
| `platform_version` | The platform version you ship with. Pass `FH3MOD_PLATFORM_VERSION`. If the installed platform is older, players get "Old FH3 Mod Platform: reinstall Name". |
| `check_updates` | 0 to never go online for your mod. Give players a setting for it. |
| `description` | API 6: a sentence or two for the MODS tab's Installed mods page, or null. |
| `offline_only` | API 9: nonzero if your mod shouldn't run in online play. See [Online play](#online-play-api-9). |

The update check runs in the background. If GitHub can't be reached, the last answer is used.

## log

```c
void log(FH3Mod* mod, const char* utf8_line);
```

Writes one line to `fh3-mods\platform.log`, with a timestamp and `[YourMod]` in front. The log starts fresh at every launch (a copy of each session's is kept in `fh3-mods\platform-logs`). Keep it short by default: log start-up problems always, and the details only when the player turns a `Log`/`DebugLog` switch of yours on, as the platform does with `platform.ini` `[log] Verbose`. A mod with a lot to say should keep its own log as well ([Sessions](#sessions-api-9)).

## set_string, replace_string, clear_strings, apply_strings (FH2 tables)

> **Not for FH3.** These edit FH2's string tables (the "LSB2" format with 16-bit keys). FH3's tables (`media\Stripped\StringTables\<language>.zip`) use 32-bit hashed keys, so in FH3 these calls change nothing. To give your mod texts in FH3, add new keys with [`add_string_key`](#string-table-keys-api-10) and refer to them (the upgrade bridge does this for its names and descriptions). Replacing an existing FH3 text isn't supported yet.

### set_string

```c
void set_string(FH3Mod* mod, const char* table, uint16_t key, const wchar_t* text);
```

Sets the string with `key` in `table` (for example `"Main.str"`) to `text`, in every language. To find keys, see [`string-tables.md`](string-tables.md). If two mods set the same key, the one loaded later wins, and the log says so.

### replace_string

```c
void replace_string(FH3Mod* mod, const char* table, const wchar_t* from, const wchar_t* to);
```

Replaces every string in `table` that is exactly `from` (the whole string, case-sensitive) with `to`. This runs after key edits, so it sees their results. It's handy for text that's the same in every language, like the names in `AIPlayers.str`.

### clear_strings

```c
void clear_strings(FH3Mod* mod, const char* table);
```

Drops your edits to `table`, or to every table if `table` is null. When no mod edits a table any more, its override files are removed, so the game uses its own text again. That happens straight away when you call `clear_strings` and then `apply_strings`. For a table left over from an earlier launch, such as one from a mod that has since been removed, it happens about 15 seconds after startup, which leaves mods time to make their edits from their own threads first.

### apply_strings

```c
void apply_strings(FH3Mod* mod);
```

Writes every table that changed since the last write. It's done for you once all mods have returned from `fh3mod_init`, so call it yourself only for edits you make later. The game reads its string tables once, at startup, so a later edit shows from the next launch.

Edits are written to the `mediaoverride` folder, as `mediaoverride\Stripped\StringTables\<language>\<table>`. Extra Options makes the game read that folder; when it isn't loaded, the platform does it ([fh3-game-notes.md](fh3-game-notes.md#override-folder-mount-mechanism-read-from-code-and-the-vtable-slots-checked-against-the-exe-on-disk)). With Extra Options loaded and `EnableMediaOverrides=0`, the files are still written but the game won't read them. The tables the platform wrote are listed in `fh3-mods\platform-tables.txt`, so that ones no longer needed get cleaned up.

## set_notice

```c
void set_notice(FH3Mod* mod, const char* utf8_lines);
```

On FH2 this puts lines on the title screen. **In FH3 the lines are kept** (`fh3-mods\notices\<name>.txt`) **but not shown**: the title screen text is in a hashed table. Update checks don't need it: a newer release of your mod shows on **MODS > Updates**. Tell the player about problems with your log, or a settings row's description.

## game_base and wait_for_game

```c
void* game_base(void);
void* wait_for_game(uint32_t timeout_ms);
```

`game_base` returns the base address of `forza_durango_release_final.exe` once Azimuth has mapped it into the process, or null before that. Azimuth maps it when the game starts. `wait_for_game` waits until it's mapped, or until `timeout_ms` runs out (`0xFFFFFFFF` waits forever), and returns the base or null. It checks every 100 ms. Call it from your own thread, never from `fh3mod_init`, which would hold up every mod loaded after you.

The game is recognised by its PE header: the Xbox One release of 5 June 2018 (1.0.125.0), timestamp `0x5B16ECCA` and image size `0x05495000`. Azimuth maps it at its preferred base, `0x140000000`, but use the returned address anyway. Every `game+0x...` address in these docs and in the mods is relative to that base.

**Other games.** Azimuth runs other Forza games from the same launcher, and the platform and your mod load before the game is known. Their exes have the same name and are mapped at the same address. Once Azimuth has mapped another game, or FH3 of another build, `game_base` returns null for the rest of the session, and `wait_for_game` and `wait_for_code` return null straight away. So treat null as "there's no FH3 to change" and stop there, as for a timeout. The log says which game was started.

## set_file and clear_file (API 2)

```c
int set_file(FH3Mod* mod, const char* media_path, const void* data, size_t size);
void clear_file(FH3Mod* mod, const char* media_path);
```

`set_file` replaces a file the game reads from `game:\media`. Give the path relative to `media`, with forward or back slashes. For a file inside a zip, use the zip's name without `.zip` as a folder: `media\UI.zip\x.xml` is `"UI/x.xml"`. The file is written straight away to the `mediaoverride` folder, which Extra Options mounts over `media`, or the platform when Extra Options isn't loaded. Textures are `.swatchbin` files; the upgrade icons of the mods are made with `scripts/gen_icons.py` in their repos. If the file there already has the same content, it isn't rewritten.

Only one mod can replace a file. `set_file` returns 1 when the file is yours and written. It returns 0 if another mod already replaces it, if the path isn't a plain path under `media`, or if the write failed, and the log says which. String tables and the game database can't be replaced whole; use the functions for those. If someone had put a file at that spot by hand, it's kept next to it as `<name>.bak` the first time.

`clear_file` deletes your override, so the game reads its own file again. Pass null to clear every file you replace. The files the platform wrote are listed in `fh3-mods\platform-files.txt`. About 15 seconds after startup, any listed file that no mod has claimed this launch is deleted. That's what cleans up after a mod that was removed.

The game reads most files once, when it first needs them, so call `set_file` early, from `fh3mod_init` or soon after.

## add_sql, clear_sql and apply_database (API 2)

```c
void add_sql(FH3Mod* mod, const char* utf8_sql);
void clear_sql(FH3Mod* mod);
void apply_database(FH3Mod* mod);
```

`media\Stripped\gamedbRC.slt` is a SQLite database that holds most of the game's data: cars (`Data_Car`), events, AI drivers and around 280 more tables. `add_sql` queues statements to run against it. One call can hold several statements separated by semicolons, and calls add up.

The platform copies the database the game would read and runs every mod's statements, in load order, each mod inside its own transaction. If any statement from a mod fails, all of that mod's changes are rolled back and the SQLite error is logged, while the other mods' changes still apply. The result goes to `mediaoverride\Stripped\gamedbRC.slt`, which the game reads first.

Another mod's own `gamedbRC.slt` in `mediaoverride\Stripped` (a car pack, say) is built on, and a copy of it is kept as `fh3-mods\platform-db-base.slt`; otherwise a car pack's in `mediapc\Stripped`, and without one the game's original. When no mod has statements any more, the platform's copy is deleted and the other mod's database is put back, byte for byte.

Every copy the platform builds has the platform's mark in its header (`application_id` 0x46483244; the game doesn't read it), and `fh3-mods\platform-db.txt` has its size and date. So don't delete `mediaoverride\Stripped\gamedbRC.slt` in your uninstall steps (the platform cleans up its own), and don't ship the platform's copy in your mod. Building the copy takes a moment, so it's only redone when something changed: `platform-db.txt` remembers a hash of the base and every mod's statements.

Useful tables: `Data_Car` (cars, `Id` is the Data_Car id the car code uses, `BaseCost`), `Data_CarBody` (track widths, ride heights), `List_Upgrade*` (parts and prices), `UpgradeTypes` / `Upgrades` / `UpgradeAreas` (the shop's lists), and the save's `Profile0_*` tables, which live in the profile database, not here.

Statements added in `fh3mod_init` are applied for you. For later ones, call `apply_database` from your own thread. Like string tables, the database is read once at startup, so later changes show from the next launch.

The platform has SQLite built in. If your mod reads a SQLite file itself, build SQLite in too rather than loading `winsqlite3.dll`, or it won't work for Linux players ([linux.md](linux.md)). To look around the database, open a copy of it in any SQLite browser.

## Interfaces between mods (API 2)

```c
void publish_interface(FH3Mod* mod, const char* name, uint32_t version, const void* iface);
const void* get_interface(const char* name, uint32_t min_version);
int wait_for_mods(uint32_t timeout_ms);
const char* mod_version(const char* name);
```

A mod can offer other mods a struct of function pointers. Publish it under a name, usually `"YourMod.Something"`, with a version number. Ship a small header that defines the struct, and treat it the way the platform treats `FH3ModApi`: start it with a `size` member, never change or remove a member, add new ones only at the end, and bump the version when you do. `iface` must stay valid for as long as the process runs. A name belongs to the first mod that publishes it. Publishing null withdraws it.

`get_interface` returns the struct if it's published with at least `min_version`, or null. Mods start in alphabetical order, so the mod you want may not have published yet while your `fh3mod_init` runs. `wait_for_mods` waits until every mod's `fh3mod_init` has returned, and returns 1 once they have. Call it from your own thread; from inside `fh3mod_init` it returns 0 straight away, since waiting there would hang every mod after yours.

`mod_version` gives the version a mod registered with, matched by name without regard to case, or null if it isn't installed. Use it to check for a mod you depend on and to tell the player, perhaps with `set_notice`, when it's missing or too old.

No FH3 mod publishes an interface yet. For the player's car, use the upgrade bridge's calls rather than another mod's interface: every mod then gets the same answer.

## Hooks (API 3)

```c
FH3Hook* hook_function(FH3Mod* mod, const char* name, void* target, void* detour, void** original);
FH3Hook* hook_call(FH3Mod* mod, const char* name, void* call_site, void* detour, void** original);
FH3Hook* hook_pointer(FH3Mod* mod, const char* name, void** slot, void* detour, void** original);
int enable_hooks(FH3Mod* mod, FH3Hook* hook);
int disable_hooks(FH3Mod* mod, FH3Hook* hook);
void* wait_for_code(uint32_t timeout_ms);
```

These change what game code does, and they're the way to do it when more than one mod might touch the same code. The platform keeps one list of every hook from every mod. It checks each new hook against that list and against the game as shipped, and it writes the patches safely while the game's threads are running. Function hooks are made with [MinHook](https://github.com/TsudaKageyu/minhook), which is built into the platform, so your mod doesn't need a copy of its own.

There are three kinds:

- **`hook_function`** puts an inline hook at the start of a function, so every call to it goes to your detour.
- **`hook_call`** retargets one `CALL rel32` (`E8`) or `JMP rel32` (`E9`) instruction, so only that call goes to your detour, and other callers of the same function aren't affected. `*original` is the function it called. Your detour can be anywhere: the platform puts a stub within reach of the call.
- **`hook_pointer`** replaces a function pointer in memory: a vtable slot, a callback in a table, an import. It has to be 8-byte aligned. `*original` is the pointer's old value.

A call site's 4-byte target is swapped in one atomic write where it fits in an aligned 8 or 16 bytes. Otherwise the platform pauses the other threads for the moment it takes, and makes sure none of them is on that instruction.

Each call returns a hook that is switched off, or null with the reason in the log. `name` appears in the log and in crash reports, so name what you hooked: `"FMOD::System::createSound"`. The detour must have exactly the signature and calling convention of the code it replaces. `*original` is set before the call returns. Call it from your detour to run the replaced code, which includes any other mod's detour that runs after yours. It's safe to call at any time. While your hook is switched off, it goes straight to the game's own code, past any other mod's detour.

`enable_hooks` and `disable_hooks` switch one hook, or all of yours if you pass null. Switching several at once is better, because MinHook pauses the game's threads once for all of them, not once each. They return 1 if every hook is now switched the way you asked, and 0 if any couldn't be, with the reason in the log. When the last hook on a place is switched off, the game's original bytes go back in.

**Several mods on one function.** When two mods hook the same function, call site or pointer, both hooks run. The most recently enabled detour runs first, and its `original` leads to the next one, and so on down to the game's own code. Any hook can be switched off without breaking the others: the chain closes up around it. Hooks that only overlap are refused, whatever their kind and whoever made them: a function hook on code that contains another mod's call hook, or a second function hook two bytes into a hooked function. The log names the mod already there.

**Game code that was already changed.** Before hooking game code, the platform compares the bytes it would change (or, for a function, the bytes MinHook copies) with `forza_durango_release_final.exe` in the game folder. If they differ, something outside the platform already patched them (Extra Options, or a mod patching the game by itself), and the hook is refused rather than layered on top. Pointers are only compared when the exe has an address in the image there, such as a vtable slot. Imports and tables the game fills in at runtime can't be checked this way, so they're allowed. Extra Options changes around 130 places in the game, listed in its `patches.json`. If you need one of those, talk to its author.

**wait_for_code.** Hooks in game code can't be made until `wait_for_code` has returned. It waits like `wait_for_game` and then, if Extra Options is installed, also until Extra Options has finished patching. Extra Options checks every place it patches before it changes any of them, and if a single one isn't as it expects, it patches nothing at all, so a mod hooking early could quietly switch Extra Options off. The platform knows Extra Options is done when its log (`FH3_ExtraOptions.log`, written this launch) says so. That's the status line it writes once it's done (`OK:`, `REFUSED:` or `DISABLED:`), or its `AZIMUTH: winmm proxy initialized` line. If it hasn't said so 20 seconds after the game was mapped, the platform goes ahead anyway and logs that. Hooks outside the game (in Azimuth or a Windows DLL) don't have to wait.

**Other games.** When Azimuth has started another game (see [game_base](#game_base-and-wait_for_game)), `wait_for_code` returns null, and every hook or patch into that game's exe is refused, whatever address you pass.

```cpp
// Car::Update (game+0x18B8D20) runs once a frame for every car, on the game's thread (Airride hooks it).
using CarUpdateFn = uint64_t (*)(void* car, float dt);
static CarUpdateFn g_car_update;

static uint64_t my_car_update(void* car, float dt) {
    const uint64_t r = g_car_update(car, dt);
    // ... look at the car (Car+0x140: its physics spec, Car+0x110: the record whose +0x30 is the Data_Car id) ...
    return r;
}

static DWORD WINAPI start(LPVOID) {
    char* base = static_cast<char*>(g_api->wait_for_code(0xFFFFFFFFu));
    if (!base) return 0;   // not FH3, or not this build
    if (FH3Hook* h = g_api->hook_function(g_mod, "Car::Update", base + 0x18B8D20,
                                          reinterpret_cast<void*>(&my_car_update), reinterpret_cast<void**>(&g_car_update)))
        g_api->enable_hooks(g_mod, h);
    return 0;
}
```

Start such a thread from `fh3mod_init` (`CreateThread`), never wait in `fh3mod_init` itself.

With `platform.ini` `[log] Verbose=1` every hook is logged when it's made and switched (`[MyMod] hooked function game+0x18B8D20 (Car::Update)`); problems are always logged, and crash reports list every hook, marking the ones that were on. A crash inside a MinHook trampoline (the copy of the game's first instructions) is reported as such, with the hook it belongs to.

## Byte patches and instruction hooks (API 7)

```c
FH3Hook* patch_bytes(FH3Mod* mod, const char* name, void* at, const void* expect, const void* bytes, uint32_t size);
FH3Hook* hook_instruction(FH3Mod* mod, const char* name, void* at, FH3InstructionFn fn, void* user);

typedef struct FH3Context {
    uint64_t rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rflags;
    uint64_t reserved;
    FH3Xmm xmm[16];             /* .f[4], .d[2], .u32[4], .u64[2] */
} FH3Context;
typedef void (*FH3InstructionFn)(FH3Context* ctx, void* user);
```

For when a hook on a whole function, one call or one pointer is too blunt: a constant in the middle of a long function, a branch to flip, the value a register holds at one exact instruction. Both come back as an `FH3Hook`, switched off, and are switched with `enable_hooks` and `disable_hooks` like the hooks above. The same rules hold: hooks in game code wait for `wait_for_code`, the bytes are checked against `forza_durango_release_final.exe` and against every other hook, and everything is logged and listed in crash reports. Check `api_version >= 7`.

Addresses here are exact instructions, so they belong to the game build you found them in (the one Azimuth runs). If the bytes aren't what you expect, `expect` and the check against the exe on disk refuse the change rather than writing over something else.

**patch_bytes** writes `size` bytes (1 to 32) of `bytes` at `at`. Pass the bytes you expect to find there in `expect` (the same size), or null to take whatever is there as long as it matches the exe on disk. Switching the patch off puts the original bytes back. Only one patch can cover a byte, and a patch can't overlap any hook. The bytes are written with the game's other threads paused, none of them half way through them.

```cpp
// (made-up address and bytes, to show the call) a conditional jump made unconditional
static const unsigned char was[] = {0x74, 0x12}, now[] = {0xEB, 0x12};   // je +0x12 -> jmp +0x12
if (FH3Hook* p = api->patch_bytes(mod, "always take the branch", base + 0x1234567, was, now, sizeof now))
    api->enable_hooks(mod, p);
```

**hook_instruction** calls `fn(ctx, user)` every time the game reaches the instruction at `at`, before that instruction runs. `ctx` holds every general-purpose register, the flags and `xmm0` to `xmm15` as they are at that point. Change any of them (except `rsp`, which is there to read) and the game carries on with your values. `at` can be any instruction in a function, not just its first, as long as:

- it's the start of an instruction (the platform decodes the function from its start to make sure);
- it's past the function's prolog, the instructions that set up its stack frame;
- the hook's jump fits: it replaces 5 bytes, so the instructions covering them have to stay inside the function, and nothing in the function may jump into them other than to `at` itself.

When it refuses, the log says which of these it was. Inside, the replaced instructions are moved to a stub of the platform's, rewritten where they refer to nearby code or data (rip-relative operands, `CALL`, `JMP` and conditional jumps), and run there after `fn`, before the game continues after them. The stub has unwind information chained to the game function's own, so a stack walk from inside `fn`, or in a crash report, carries on through to the game function's callers. On a CPU with AVX the upper halves of `ymm0` to `ymm15` are kept too, so a `fn` compiled for AVX can't disturb the game's 256-bit values.

Several mods can hook the same instruction; all of their `fn`s run, the most recently enabled first, each seeing the changes of the ones before it. `fn` runs on the game's thread, often every frame, so keep it short. If it crashes, the crash is caught, logged under your mod's name, and that hook is skipped from then on; the game keeps running. Instruction hooks work in the game and in any loaded DLL with unwind information, like your own.

```cpp
// Universal Wheel Spacers: the central car loader (game+0x19E6A80) has just stored the car's track widths into the
// spec it fills (r14 + 0xF4 front, + 0xF8 rear, metres); [rsp+0x68] holds the Data_Car id it was given.
static void track_stored(FH3Context* ctx, void*) {
    char* spec        = reinterpret_cast<char*>(ctx->r14);
    const int car_id  = *reinterpret_cast<const int*>(ctx->rsp + 0x68);
    const int stage   = g_api->upgrade_stage_for_car(g_mod, g_front, car_id);   // 0: not the player's car
    if (stage > 0) *reinterpret_cast<float*>(spec + 0xF4) += 2.0f * stage * 0.01f;
}

if (api->api_version >= 7)
    if (FH3Hook* h = api->hook_instruction(mod, "Car spec loaded (track outer)", base + 0x19E9027, &track_stored, nullptr))
        api->enable_hooks(mod, h);
```

Prefer `hook_function`, `hook_call` or `hook_pointer` when one of them can do the job: they don't depend on the exact instructions inside a function. An instruction hook's register meanings come from reading the game's code at that spot, so note them next to the address, as above.

## The Mods page (API 4; sliders API 5)

```c
int add_setting(FH3Mod* mod, const FH3Setting* setting);
int get_setting(FH3Mod* mod, const char* key);
void set_setting(FH3Mod* mod, const char* key, int value);
float get_slider(FH3Mod* mod, const char* key);              /* API 5 */
void set_slider(FH3Mod* mod, const char* key, float value);  /* API 5 */

typedef struct FH3Setting {
    uint32_t size;                 /* sizeof(FH3Setting) */
    const char* key;               /* unique within your mod: "Trees" */
    const char* label;             /* the row's label: "Breakable trees" */
    const char* description;       /* beside the list while the row is selected; may be NULL */
    int value;                     /* 0 = Off, 1 = On */
    int needs_restart;             /* nonzero: a change only takes effect the next time the game starts */
    FH3SettingChangedFn on_change; /* void (*)(void* user, const char* key, int value); may be NULL */
    void* user;
    /* API 5: sliders. Zeroed, the row is On/Off. */
    int kind;                      /* FH3_SETTING_ON_OFF, _SLIDER, _LINK (API 6) or _CHOICE (API 9) */
    float min, max;                /* the slider's range */
    float step;                    /* one press of left or right; 0 = a hundredth of the range */
    float slider_value;            /* its current value */
    float display_scale;           /* the number shown is value * this (100 shows 0.12 as 12); 0 = 1 */
    int decimals;                  /* digits after the decimal point, 0 to 3 */
    FH3SliderChangedFn on_slide;   /* void (*)(void* user, const char* key, float value); may be NULL */
    /* API 6: links */
    const char* url;               /* with kind = FH3_SETTING_LINK */
    /* API 9: choices */
    const char* const* choices;    /* with kind = FH3_SETTING_CHOICE: the names, UTF-8 */
    uint32_t choice_count;         /* 2 to FH3_SETTING_MAX_CHOICES (32) */
} FH3Setting;
```

These put On/Off switches, sliders, links and choices for your mod in the game's own menus: **Home > MODS > Installed mods**, A on your mod opens its settings ("SETTINGS / YOUR MOD"), and **All settings** lists every mod's rows. It's laid out like the game's HUD options page: the row's description, followed by your mod's name, shows beside the list. The player can also switch a whole mod off on Installed mods (it then isn't loaded from the next start).

Add your rows from `fh3mod_init`. The platform writes the page once every mod's `fh3mod_init` has returned, so a row added after that only shows from the next launch (the log says so). `key` is letters, digits and `_`, and unique within your mod. `add_setting` returns 1, or 0 with the reason in the log.

**Your mod owns the value.** The platform doesn't store it anywhere. Pass the current value in `value`, from your ini or wherever you keep your settings. When the player changes the row, `on_change` gets the new value with your `user` pointer: save it, and start using it if it can change on the fly. `on_change` runs on the game's UI thread, inside the menu's own code, so keep it short and don't call into the game from it. A crash in it is caught and logged. `get_setting` returns a row's current value (-1 if you have no row called `key`). If you change a value yourself, from a hotkey say, `set_setting` makes the page show it the next time it opens; `on_change` isn't called for that.

**Switches that need a restart.** Some changes can only take effect when the game starts, like anything the game reads once while loading. Mark those rows `needs_restart`. Their description then ends with "Takes effect the next time you start the game." While any of them differs from the value the game started with, a notice under the list says so. When the player leaves the Mods page with such a change, the game's own message box offers **Restart now**, **Restart later** and **Go back**. Restart now closes the game the way Azimuth's own Quit does, then starts Azimuth again with `--skip-launcher`, straight back into the game. Your `on_change` has already been called by then, so save there and the restart picks up the new value.

```cpp
static void changed(void*, const char* key, int value) {
    WritePrivateProfileStringA("MyMod", key, value ? "1" : "0", g_ini.c_str());
    if (std::strcmp(key, "Sparks") == 0) g_sparks = value;   // takes effect at once
}

// in fh3mod_init, after register_mod:
if (api->api_version >= 4) {
    FH3Setting s{sizeof s};
    s.key = "Sparks"; s.label = "Sparks"; s.description = "Sparks fly when you scrape a wall.";
    s.value = g_sparks; s.on_change = changed;
    api->add_setting(mod, &s);
}
```

**Sliders (API 5).** Set `kind = FH3_SETTING_SLIDER` and the row is the game's own slider, like the audio volumes: left and right move it by `step` between `min` and `max`, and the number at its end is the value times `display_scale`, with `decimals` digits after the point. A value kept as a share, like 0.12, reads best with `display_scale = 100` and no decimals ("12"). Pass the current value in `slider_value`; `on_slide` gets each new one, already kept inside the range and on a step. `get_slider` and `set_slider` work like `get_setting` and `set_setting`. `needs_restart` works for sliders too. An older platform would show a slider as an On/Off row, so only add one when `api_version >= 5`:

```cpp
static void slid(void*, const char* key, float value) {
    char v[32];
    std::snprintf(v, sizeof v, "%g", value);
    WritePrivateProfileStringA("MyMod", key, v, g_ini.c_str());
    if (std::strcmp(key, "SparkAmount") == 0) g_spark_amount = value;
}

if (api->api_version >= 5) {
    FH3Setting s{sizeof s};
    s.key = "SparkAmount"; s.label = "Spark amount"; s.description = "How many sparks fly, in percent.";
    s.kind = FH3_SETTING_SLIDER; s.min = 0; s.max = 2; s.step = 0.05f;
    s.slider_value = g_spark_amount; s.display_scale = 100; s.on_slide = slid;
    api->add_setting(mod, &s);
}
```

**Choices (API 9).** Set `kind = FH3_SETTING_CHOICE` and give `choices` and `choice_count`, and the row shows one of them by name, between the same left and right arrows as the game's own option rows ("Speedometer: Analogue / Digital"). Left and right step through them, stopping at the ends. `value` is the index of the one chosen, from 0, and `on_change`, `get_setting` and `set_setting` use that index (`set_setting` keeps it in range). The names are copied when you add the row; there can be 2 to 32 of them, and a dozen or so characters of each fit. `needs_restart` works as for a switch. Use it wherever a number would otherwise stand for something with a name, like a button or a mode. An older platform would show it as an On/Off row, so only add one when `api_version >= 9`:

```cpp
static const char* const kButtons[] = {"LB", "RB", "A", "B", "X", "Y"};

if (api->api_version >= 9) {
    FH3Setting s{sizeof s};
    s.key = "Button"; s.label = "Nitro button"; s.description = "The button that burns nitro.";
    s.kind = FH3_SETTING_CHOICE; s.choices = kButtons; s.choice_count = 6;
    s.value = g_button; s.on_change = changed;   // changed() gets the index: 0 for LB, 1 for RB...
    api->add_setting(mod, &s);
}
```

**Links (API 6).** `kind = FH3_SETTING_LINK` with a `url` makes a row that opens the address in the browser when the player presses A on it (only on your mod's own page).

The log says when the page is ready, and each change is logged under your mod's name. If the game's menu code doesn't look as expected, there's no MODS tile and your mod keeps running without being told of changes. [`fh3-game-notes.md`](fh3-game-notes.md) and [`mods-page-fh3-map.md`](mods-page-fh3-map.md) explain how the page is put together.

## Pages and title-screen entries (API 8, 9): FH2 only

```c
int add_page(FH3Mod* mod, const FH3Page* page);                 /* + set_page_rows, set_page_xaml, set_page_slot, ... */
int add_title_entry(FH3Mod* mod, const FH3TitleEntry* entry);   /* + title_started_by */
```

On FH2 a mod can add pages of its own to the menus (rows or its own XAML, on a pause-menu tile) and lines with a button on the title screen. They need the addresses of FH2's menu and title code, which haven't been found for FH3 yet, so **in FH3 `add_page` and `add_title_entry` return 0** and the log says so; the other page calls then have no page to work on. The structs are in the header, so a mod that uses them on FH2 compiles for FH3 too. For settings, use the [MODS page rows](#the-mods-page-api-4-sliders-api-5); for something on the car's tuning page, [tuning rows](#tuning-rows-api-13).

## Online play (API 9)

```c
int playing_online(void);
/* and in FH3ModInfo: */
int offline_only;
```

On FH2, FH2MP brings back online play, and a mod with `offline_only = 1` isn't loaded when the game is started online. FH3 has no such online play: **`playing_online` always returns 0** and `offline_only` changes nothing. Set it anyway if your mod would be a cheat online (a boost, say): the mods here do, so they stay out if FH3 ever gets online play on the platform. Call `register_mod` first in `fh3mod_init` and return when it gives you `NULL`.

## Sessions (API 9)

```c
const char* session_id(void);
const wchar_t* session_log_path(FH3Mod* mod, const wchar_t* folder, uint32_t keep);
```

Each run of the game is a session, with an id like `20261021-203224-1234`: the time the platform started, and the process id. The game can start twice in one launch, and the MODS page's Restart now starts a new process, so "the newest log" isn't always the one you want; the id says which run a file belongs to.

- **`platform.log`** is still rewritten every time the game starts, and its third line names the session ("session 20261021-203224-1234: started 2026-10-21 20:32:24, process 1234; ..."). The platform also keeps a copy of each session's log in `fh3-mods\platform-logs\platform-<id>.log`, the newest 20.
- **`session_id`** returns the id. Call it from anywhere, `fh3mod_init` included.
- **`session_log_path`** makes `<azimuth_dir>\<folder>\logs\` and returns `<folder>\logs\<id>.log` in it, for your own log: give your mod's own folder next to `azimuth.exe` (`L"MyMod"`), and how many logs to keep there, counting this one. Older `*.log` files in that `logs` folder are deleted. Open the file yourself (the platform doesn't create it); the path stays valid for the session. It returns `NULL`, and the log says why, if `folder` isn't a plain folder name or can't be made.
- **`fh3-mods\platform-session.json`** says what's running, for tools such as a test runner or a log collector. It's written to a `.tmp` and moved over, so a reader never sees half of it, once the mods have started and again when something in it changes:

  ```json
  {
    "session": "20261021-203224-1234",
    "pid": 1234,
    "started": "2026-10-21 20:32:24",
    "platform": "1.0.0",
    "azimuth": "D:\\Games\\Azimuth",
    "mods": {
      "NitroSystem II": "1.0.0"
    },
    "pipe": "\\\\.\\pipe\\fh3mod-1234",
    "state": "running"
  }
  ```

  `mods` has every mod that registered. `pipe` is the command pipe (see Commands), or `""` while it isn't up. `state` is `running`, or `crashed` after a crash, with `"crash_report": "crashes\\crash-....txt"` (relative to `fh3-mods`). Azimuth ends the game with `TerminateProcess`, so nothing can write `ended`: a tool should take a `running` whose `pid` isn't alive any more (or whose process started at another time than `started`) as a session that's over.

## Save snapshots (API 9)

```c
int request_save_snapshot(FH3Mod* mod, const char* reason, uint32_t keep);
```

Before an experimental session, the safe thing is a copy of the saves. Ask the platform for one from `fh3mod_init` (a mod being tested, or one that changes what gets saved), and once every mod's `fh3mod_init` has returned it copies the saves folder (`<azimuth_dir>\data\titles\<title id>\saves`, the id from `data\azimuth.ini`) to `fh3-mods\save-snapshots\<session id>\`: once, however many mods ask, keeping the newest `keep` copies (the most any mod asked for). `platform.log` says how much it copied and for which mods and reasons. The game only reads the saves when A is pressed on the title screen, long after.

The platform only ever reads the saves. To go back to a snapshot, close the game and copy the whole snapshot over the saves folder: Azimuth checks the saves' hashes, so don't mix files from different copies. It returns 1 if a copy will be made, 0 if there's no saves folder or the call came after `fh3mod_init` (the log says which).

## Hotkeys (API 9)

```c
int add_hotkey(FH3Mod* mod, const FH3Hotkey* hotkey);

typedef struct FH3Hotkey {
    uint32_t size;                 /* sizeof(FH3Hotkey) */
    const char* key;               /* unique within your mod: "Mark" */
    const char* label;             /* "Mark this spot", for its rows on the Mods page */
    uint32_t vk;                   /* a Windows virtual-key code (VK_F9, 'M'); 0 = none */
    uint32_t modifiers;            /* FH3_KEY_CTRL | FH3_KEY_SHIFT | FH3_KEY_ALT */
    uint32_t pad_chord;            /* FH3_PAD_LB | FH3_PAD_RB | FH3_PAD_Y ...; 0 = none */
    FH3HotkeyFn on_press;          /* void (*)(void* user, const char* key) */
    FH3HotkeyRebindFn on_rebind;   /* void (*)(void* user, const char* key, uint32_t vk, uint32_t modifiers, uint32_t pad_chord) */
    void* user;
} FH3Hotkey;
```

A key on the keyboard, a chord on the controller or both, that call `on_press` once per press: a key that writes "the problem is here" to your log, or a live switch while you test something.

- **Only while the game is in front.** The platform polls the keyboard (`GetAsyncKeyState`) and the controllers (XInput, from System32; since 1.1.0 other gamepads such as a DualSense are added to what `XInputGetState` returns to mods, see below) every 15 ms while Azimuth's window has the focus. It never takes a key from other programs (no `RegisterHotKey`), and the game still gets every key and button, so pick ones that do nothing harmful in game: an F-key, or LB+RB with a face button. The modifiers have to match exactly, so Ctrl+F5 doesn't set off a hotkey on F5.
- **`on_press`** runs on the platform's hotkey thread, not the game's. Keep it short, and hand anything that touches the game to a hook. If it crashes, the log says so and the next press calls it again.
- **The Mods page.** Each hotkey gets two choice rows on your mod's settings page (Home > MODS > Installed mods): "<label>: key" (Off, F1 to F12, Ctrl+F1 to F12, or your own binding if it's none of those) and "<label>: controller" (Off, or LB+RB with Y, X, B, A or a d-pad direction, or your own chord). A change there takes effect straight away and calls `on_rebind` with the new binding. The platform doesn't keep bindings: save them in your settings and pass them in next time, as with your rows.
- **When.** Add hotkeys from `fh3mod_init`, so their rows show (rows added later only show from the next launch). It returns 1 if the hotkey was added, 0 if not, and the log says why.

```cpp
FH3Hotkey mark{sizeof mark};
mark.key       = "Mark";
mark.label     = "Mark this spot";
mark.vk        = saved_vk ? saved_vk : VK_F9;
mark.pad_chord = FH3_PAD_LB | FH3_PAD_RB | FH3_PAD_Y;
mark.on_press  = [](void*, const char*) { g_mark_now = true; };   // the frame hook writes the position
mark.on_rebind = [](void*, const char*, uint32_t vk, uint32_t mods, uint32_t chord) { save_binding(vk, mods, chord); };
api->add_hotkey(g_mod, &mark);
```

## Commands (API 9)

```c
int add_command(FH3Mod* mod, const char* name, const char* help, FH3CommandFn fn, void* user);
typedef void (*FH3CommandFn)(void* user, const char* name, const char* args, char* reply, size_t reply_size);
```

A command is something another program can ask your mod to do while the game runs: flip a setting for an A/B test without restarting, print some numbers, mark a spot. Run them with `fh3mod.exe`, in the `tools` folder of the platform's zip:

```
fh3mod list
fh3mod MyMod shadows off
fh3mod --pid 1234 MyMod stats
```

- **`add_command`** registers `name` (letters, digits and `_`, unique within your mod, matched in any case) with a line of `help` for `fh3mod list` (or `NULL`). Add commands whenever you like; it returns 1 if added, 0 if not, and the log says why.
- **`fn`** runs on the platform's command thread, not the game's, one command at a time, with the rest of the line in `args`. Write your answer into `reply` (UTF-8, `reply_size` bytes with the NUL: 16 KB). Start it with `error: ` for a failure: `fh3mod.exe` prints it and exits with 1. Hand anything that touches the game to a hook or your own thread. If `fn` crashes, the request gets an error and the log says so.
- **The pipe.** It starts when the first command is added: `\\.\pipe\fh3mod-<process id>`, also in `fh3-mods\platform-session.json`. It refuses remote clients, and its DACL lets in only the Windows user the game runs as, so only that user's programs on that PC can connect. A request is one line of UTF-8: `list`, or `<mod> <command> [args]`, with a mod name that has spaces in double quotes. The reply is text, and the connection then closes. Any tool can use it the way `fh3mod.exe` does (`tools/fh3mod.cpp`).
- **Which game.** Without `--pid`, `fh3mod.exe` finds the one running game with a command pipe; when there are more (the game started twice, or two installs), it lists their process ids for `--pid`.
- **On Linux** the pipe is inside the game's Wine prefix, so run `fh3mod.exe` with the same prefix (`wine`, or `protontricks -c`).

```cpp
api->add_command(g_mod, "debris", "debris clear: removes every broken piece now",
                 [](void*, const char*, const char* args, char* reply, size_t size) {
                     if (strcmp(args, "clear") != 0) { snprintf(reply, size, "error: debris clear"); return; }
                     g_clear_debris = true;   // the game's update hook does it
                     snprintf(reply, size, "clearing");
                 }, nullptr);
```

## String-table keys (API 10)

```c
void add_string_key(FH3Mod* mod, const char* table, uint32_t key, const wchar_t* text);
```

FH3's database-backed texts (names and descriptions of upgrades, for example) live in string tables whose keys are 32-bit hashes, not the small numbers `set_string` takes. `add_string_key` adds `text` to the table named `table` (a file such as `"Upgrades.str"`) under a key you choose; pick one unlikely to clash, for example a hash of your mod's id and the text. A key the table already has is left alone. Like `set_string` the text is written for every language, applied after `fh3mod_init` and shows from the next launch. The database refers to a text as `"_&"` followed by the decimal of `((uint64_t)table_hash << 32) | key`; `FH3_TABLE_UPGRADES` (`0x968719E9`, `Upgrades.str`) and `FH3_TABLE_UPGRADETYPES` (`0x07710821`, `UpgradeTypes.str`) are the two tables upgrade names and descriptions are in. Mods that use `register_upgrade` don't need this: the bridge writes those texts itself.

## Custom upgrades: the upgrade bridge (API 11)

A mod describes an upgrade and the platform does the rest. `register_upgrade` takes a name, a description, an icon, the upgrade area and up to ten stages (stage 0 is stock), each with a name, description, price and icons. The platform then:

- hands out the ids the game needs (part enum `47 + slot`, upgrade type, upgrade and part ids) and remembers them by your upgrade's `id` in `fh3-mods\platform-upgrades.ini`, so a mod's upgrade keeps its ids between launches and updates;
- writes the database rows and the texts (up to 8 upgrades in all, 10 stages each);
- shows the type in **Garage > Upgrades** under the area you chose, runs the basket, the purchase, the "install this setup?" prompt and Abandon like the game's own parts;
- stores each car's fitted stage and the stages it owns, per garage id, in the same ini, and repairs a save that still names one of its parts when the mod is gone.

```c
typedef struct FH3UpgradeStage {
    const wchar_t* name;
    const wchar_t* description;
    int32_t price;           // credits; ignored for stage 0
    const char* icon;        // a game path (GAME:\Media\UI\Textures\...swatchbin) or NULL for the default
    const char* icon_large;  // the large one, or NULL
} FH3UpgradeStage;

typedef void (*FH3UpgradeStageFn)(void* user, uint32_t garage, int stage);

typedef struct FH3UpgradeDef {
    uint32_t size;               // sizeof(FH3UpgradeDef)
    const char* id;              // stable key, e.g. "nitrosystem2.kit"
    const wchar_t* name;
    const wchar_t* description;
    int32_t area;                // 1 Engine (0 = 1), 4 Tires and rims, 8 Conversion
    const char* icon;
    const char* icon_large;
    int32_t stage_count;         // 2 to 10
    const FH3UpgradeStage* stages;
    FH3UpgradeStageFn on_stage;  // may be NULL
    void* user;
    int32_t preferred_slot;      // -1 = any; a mod moving an older upgrade over asks for the slot it had
    int32_t camera_part;         // the game part whose shop camera the list uses (10 front rims, 11 rear rims, ...); 0 = default
} FH3UpgradeDef;

int      register_upgrade(FH3Mod* mod, const FH3UpgradeDef* def);
int      upgrade_stage(FH3Mod* mod, int handle, int32_t garage);
uint32_t upgrade_active_garage(void);
void     upgrade_import(FH3Mod* mod, int handle, int32_t garage, int32_t ordinal, int stage, uint32_t owned_mask);
int32_t  upgrade_car_model(int32_t garage);   // API 12
int32_t  upgrade_garage_for_car(int32_t car_id, uint32_t flags);       // API 14
int      upgrade_stage_for_car(FH3Mod* mod, int handle, int32_t car_id); // API 14
int      upgrade_preview_stage(FH3Mod* mod, int handle);   // API 15
uint32_t upgrade_shop_serial(void);                        // API 15
int      add_tuning_row(FH3Mod* mod, const FH3TuningRow* row);   // API 13
```

- `register_upgrade` (call it from `fh3mod_init`) returns a handle (the slot, 0 or more) or -1; the log says why. `size` must be `sizeof(FH3UpgradeDef)`.
- `camera_part` (API 12, may be left out) points the shop's camera at a part of the car while your upgrade's list is open: give the part enum of a game part that has the camera you want, e.g. `10` (front rim size: the front wheel), `11` (rear rim size: the rear wheel), `8` (tire compound), `38` / `39` (front / rear tire width). 0 keeps the area's default camera. The shop builds the preset's name from the part (`Upgrades_WheelStyle_Front` ...; the presets are in `media\Camera.zip\UICarTargetCameraPresets.xml`).
- `on_stage` is called on the game's thread when the stage on a car becomes known or changes: the player drove another car, or a stage was installed. Keep it short.
- `upgrade_stage` is the stage fitted on the car with that garage id (0 is stock), or on the car being driven if `garage <= 0`; `upgrade_active_garage` is the garage id of the car being driven (0 until known).
- `upgrade_car_model` (API 12) returns the model (car ordinal) the platform has recorded for a garage id, or 0 if unknown. A mod that finds the driven car's model itself can compare the two: when they differ the game has put the player in another car (a mission loaner), and the garage car's upgrades do not apply. NitroSystem II uses it to switch off nitro in such cars.
- **From API 14 the platform follows the player's car itself**: car switches (an event's car select), loaners and showcase cars are already in `upgrade_stage(handle, 0)`, `upgrade_active_garage` (0 when the driven car isn't the player's) and the `on_stage` calls (garage 0, stage 0 in such a car). A mod doesn't need `upgrade_car_model` for that any more.
- `upgrade_stage_for_car` (API 14) is for a hook in the game's car building: the stage of your upgrade on the car being built, by its Data_Car id, 0 when that car isn't the player's (an AI car of the same model in a one-make race, a loaner), and the previewed stage in the shop. `upgrade_garage_for_car` gives the garage id instead. Call them from the build hook itself. [car-mods.md](car-mods.md) explains how the platform decides and how to test a car mod.
- `upgrade_preview_stage` (API 15) is the stage the shop's list shows while the cursor stays on it (-1 otherwise), for an effect that isn't built into the car (the Airride lowers the showroom car); `upgrade_shop_serial` goes up with every step the list takes, so a mod that ends its preview on the back button can keep it ended until the next hover.
- `add_tuning_row` (API 13) adds a slider row with a header band to the car's Tuning > Springs page (up to 64 rows); see `FH3TuningRow` in the header. Airride's "Drop (cm)" row is one.
- `upgrade_import` moves a mod's own earlier per-car data to the platform once: car `garage` (model `ordinal`, or 0) had `stage` fitted and owns the stages whose bits are set in `owned_mask`. NitroSystem II uses it to bring its FH2-style per-car stages over.

Stage prices are charged by the platform when the player confirms the setup in the shop; stages that were bought stay owned, and switching back to an owned stage is free.

### Shop preview

While the player looks at a stage of your upgrade in the shop (moves the cursor onto it), `upgrade_stage` returns **that** stage for the car, and the shop rebuilds the car the same way it does for its own parts. So an upgrade whose effect is built into the car shows on hover, before it is bought, with no extra code. What `upgrade_stage` returns for the car in the shop (platform 1.0.0):

- **cursor on a stage**: that stage, for 300 ms after the cursor moved there. That covers the rebuild the shop starts on each cursor move (it comes within about 50 ms). Backing out of the list rebuilds the car with no call the platform can see, so a later rebuild must not get the hovered stage;
- **a stage in the basket**: that stage, until the setup is installed or abandoned, or another car is driven, like the game's own basket parts;
- otherwise the fitted stage.

So a mod should read `upgrade_stage` **inside** its car-building hook, at the moment of the build.

For your upgrade to show on hover:

- **Read `upgrade_stage` at the moment the game builds the car** (in the hook on the game's car-building code), not once in `on_stage` and keep it. `on_stage` is not called for a preview: the stage is not fitted.
- **Don't cache the stage** between builds. Reading it is cheap (no file access).
- Use `upgrade_stage_for_car(mod, handle, car_id)` with the built car's Data_Car id (API 14): it gives the preview in the shop and 0 for cars that aren't the player's. On an older platform, pass the garage id (`upgrade_active_garage()`, or `0` for the car being driven).
- Effects that only exist while driving (a nitro boost, for example) have nothing to show in the shop; they should use the fitted stage, which `on_stage` reports.

Universal Wheel Spacers is the example: its hook on the car loader asks `upgrade_stage_for_car` for the front and rear stage every time the game builds a car and adds the width to the track values, so the wheels move out as the cursor goes down the stages.

### The game's part set

The game keeps a car's parts in a set of 43 slots (part enums 0 to 42) and the car's tuning right after it. Bridge upgrades use enums 47 and up, so they are **not** in that set: the platform keeps them out of it (the game's part setter has no bounds check, and a stage of a bridge upgrade written there would overwrite tuning values such as the brake balance and the handbrake). The stage of a bridge upgrade is only what `upgrade_stage` says; don't look for it in the game's part data.

## Tuning rows (API 13)

```c
int add_tuning_row(FH3Mod* mod, const struct FH3TuningRow* row);

struct FH3TuningRow {
    uint32_t size;                 /* sizeof(FH3TuningRow) */
    const char* key;               /* unique within your mod: letters, digits and '_' */
    const char* label;             /* "Airride|Drop (cm)": header band "AIRRIDE", row "Drop", unit cm */
    const char* help;              /* the description while the row is selected; may be NULL */
    float min, max, step;          /* the slider's range and step */
    int decimals;                  /* digits after the point in the number */
    float (*get)(void* user);      /* the value to show for the car being tuned */
    void (*set)(void* user, float value);   /* the player moved the slider (on a step, inside the range) */
    int (*visible)(void* user);    /* 1 = show the row for this car, 0 = hide it; may be NULL */
    void* user;
};
```

A slider of your own on the car's **Tuning > Springs** page, after Ride Height, laid out like the game's own tuning sliders: a header band above it (title in capitals, the minimum and maximum, the unit) and the row with its value. Up to 64 rows from every mod together; the page scrolls.

- **When.** Add rows from `fh3mod_init`: the row is written into the page when the tuning menu is built. It returns 1, or 0 with the reason in the log.
- **The label.** `"Header|Row label (unit)"`: the part before `|` is the band's title, a trailing `(unit)` goes to the band. Without a `|` the band and the row share the text.
- **Your mod owns the value.** `get` is asked for the car being tuned (`upgrade_active_garage()` is that car), `set` gets each new value: store it per garage id. Both run on the game's UI thread: keep them short and don't call into the game.
- **`visible`** hides the row (and its band) on cars it doesn't apply to: the Airride shows its row only on cars that have the Airride.

```cpp
static float drop_get(void*) { return drop_for(g_api->upgrade_active_garage()); }
static void drop_set(void*, float v) { save_drop(g_api->upgrade_active_garage(), v); }
static int drop_visible(void*) { return g_api->upgrade_stage(g_mod, g_handle, 0) > 0; }

if (api->api_version >= 13 && api->add_tuning_row) {
    FH3TuningRow row{};
    row.size = sizeof row; row.key = "Drop"; row.label = "Airride|Drop (cm)";
    row.help = "How far the Airride lowers the car.";
    row.min = 1; row.max = 10; row.step = 0.1f; row.decimals = 1;
    row.get = drop_get; row.set = drop_set; row.visible = drop_visible;
    api->add_tuning_row(mod, &row);
}
```

## Crash reports

These need no code from you. If the game crashes, the platform writes `fh3-mods\crashes\crash-<date>-<time>.txt` and a minidump (`.dmp`) beside it, and adds the summary to `platform.log`. The report has the exception, the call stack as module+offset (`game+0x...`, `YourMod.dll+0x...`), the mod the crash came from, if any, and every hook in place (see above). When Azimuth was running another game, it says which, so a crash there isn't taken for a mod's. The ten newest reports are kept. Open the minidump in Visual Studio or WinDbg with your mod's debug symbols to see the exact line.

- **Your mod's state is in the dump.** The minidump has the writable data (the globals) of every DLL the platform loaded: the mods and the platform itself. Other modules' data stays out, so the dump stays small. `platform.log` says how many DLLs' data and how many added ranges went in.
- **`add_dump_memory(mod, p, n)`** (API 9) adds memory of your own to every dump from then on, for state that lives on the heap behind your globals: your tables, what your hooks keep track of. 1 byte to 64 MB a range, 64 ranges at most from every mod together. The memory has to stay allocated; a range that can't be read at the crash is skipped.
- **A full-memory dump** while you chase a crash: put `full` in `fh3-mods\platform-dump.txt`. The next dump then has the whole process, which under Azimuth is several GB, so it's named `crash-<date>-<time>-full.dmp` and only the newest one is kept. Delete the file (or the word) to go back to small dumps.
- **Symbols.** The report lists the PDB (name, GUID and age) of each loaded DLL that has a CodeView record, which is what a debugger needs to load the right symbols. Release builds with llvm-mingw are stripped and have none; build with `-gcodeview -Wl,--pdb=` and keep the PDB if you want them.
- **`fh3-mods\platform-session.json`** says `crashed`, with the report's name (see Sessions).

Wine's dbghelp writes the dumps too; whether it honours the mods' data and added ranges there hasn't been checked yet.


## Controllers other than Xbox pads (platform 1.1.0)

A mod that reads `XInputGetState` itself (from `xinput1_4.dll`, as the example mods do) also sees the PC's other gamepads: the platform reads them (Windows.Gaming.Input RawGameController) and hooks `XInputGetState` for callers that are DLLs in `fh3-mods` (and its own hotkeys). The other pad's buttons are OR-ed into a real XInput pad's, or it shows up as pad 0 when XInput has none. Sony pads use their known layout (Square/Cross/Circle/Triangle as X/A/B/Y, Create/Options as Back/Start); other pads use the usual HID order (A, B, X, Y, LB, RB, Back, Start, LS, RS); the right stick and the triggers are learned from the resting axes. The game and every other caller get XInput's answer unchanged. Nothing to do in a mod. `XInputSetState` (rumble) is not routed to these pads. Settings in `fh3-mods\platform.ini`: `[input] Enabled=0` (all off), `Raw=0` (no controller reader), `Ignore=VVVV:PPPP,...` (hex vendor and product ids to skip).
