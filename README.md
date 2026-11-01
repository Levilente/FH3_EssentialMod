# FH3 Mod Platform (work in progress)

A mod loader for **Forza Horizon 3** (the Xbox One build, run through Azimuth). It is a fork of Trinculo54's
[FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform) (MIT, see `LICENSE`), changed to recognise
FH3's exe and to leave out the parts that depend on FH2's code. Status: **first core pass, not compiled or run
yet** (see "State" below).

It loads as `DWrite.dll` (and `d3dcompiler_47.dll`) next to `azimuth.exe`, which Azimuth loads on start, forwards
every function to the real one in System32, and loads every DLL in the `fh3-mods` folder. Use it in an
Azimuth install of its own: it only runs for FH3's exe, and it is not meant to share an Azimuth folder with the
FH2 platform (one `DWrite.dll` per folder).

## State

Recognises the game by its exe header: `TimeDateStamp 0x5B16ECCA`, `SizeOfImage 0x5495000` (5 Jun 2018). The
title id is read from `data\azimuth.ini` by comparing that header with each title's exe on disk, so nothing is
hard-coded.

Expected to work (same code as FH2, only the game's identity changed):
- loading mods from `fh3-mods` (`fh3mod_init(const FH3ModApi*)`, header `include/fh3mod.h`), logging to
  `fh3-mods\platform.log`, per-session logs
- crash reports in `fh3-mods\crashes`
- hooks: `hook_function`, `hook_call`, `hook_pointer` (vtable slots), `hook_instruction`, `patch_bytes`,
  `wait_for_game`, `wait_for_code`
- hotkeys, commands (`fh3mod.exe`), mods calling each other, update checks of mods on GitHub, save snapshots

Switched off (`kGameFeatures = false` in `src/platform.hpp`), because they need addresses and data layouts that
were only found in FH2 and a wrong address would crash the game:
- the Mods page, the MODS tab, pages and title-screen entries (`add_setting`, `add_page`, `add_title_entry`
  return 0 and say so in the log)
- the `mediaoverride` mount, and with it game text, replaced files and database changes

What is known about the FH3 exe so far: [`docs/fh3-game-notes.md`](docs/fh3-game-notes.md) and
[`include/fh3game.h`](include/fh3game.h). The other docs in `docs/` are the FH2 platform's and still describe FH2.

## Build

Needs llvm-mingw (clang, for the MSVC-style `__try`) and CMake, as the FH2 platform does:

```bash
./scripts/build.sh
```

`dist/` then holds `DWrite.dll`, `d3dcompiler_47.dll`, the example mod, `fh3mod.exe` and the headers. The offline
tests of the FH2 platform were left out, since they run against FH2's exe and data.
