FH3 Mod Platform
================

The shared base for Forza Horizon 3 (Azimuth) mods. Version 1.0.0, mod API 11. It's one DLL under
two names: d3dcompiler_47.dll, which Azimuth up to 1.0.1 loads, and
DWrite.dll, which Azimuth 1.0.1 and later load (1.0.3 loads only this
one). It loads every mod in the fh3-mods folder next to azimuth.exe and
gives them game-text edits, game file and database changes, title-screen
notices, update checks, logging, a way to talk to each other, access to
the game, and shared hooks into its code.

Players: copy DWrite.dll and d3dcompiler_47.dll next to azimuth.exe (the
folder of your Forza Horizon 3 install), then start the game: the MODS
tile on the Home tab opens the mods' pages. Mods built on the platform
usually ship it in their own zips too.

Built in: speed and power units of their own (Settings > HUD and
Gameplay, under Units), SELL CAR in the garage's car menu, the upgrade
bridge mods use for garage upgrades, and a proper saving screen when
you Exit to Desktop.

Mod makers: this zip has
  d3dcompiler_47.dll        the platform, to test with
  DWrite.dll                the same file, for Azimuth 1.0.1 and later
  include\fh3mod.h          the API (C/C++)
  include\fh3game.h         names for the game addresses mods use
  examples\HelloMod.dll     an example mod; put it in fh3-mods to try it
  tools\fh3mod.exe          runs mods' commands in the running game:
                            fh3mod list, fh3mod <mod> <command> [args]
Docs, the example's source and build setup:
  https://github.com/Levilente/FH3_ModPlatform

Everything the platform does is logged to fh3-mods\platform.log. If the
game crashes, a report naming the mod involved goes to fh3-mods\crashes.

MIT License. The platform includes MinHook (BSD 2-Clause), so ship
LICENSE-MinHook.txt along with the platform's DLLs. Unofficial fan-made
software; not affiliated with Playground Games, Turn 10, Microsoft or the
Azimuth developers.
