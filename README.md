# FH3 Mod Platform

A mod loader for **Forza Horizon 3** (the Xbox One build on PC, run through the **Azimuth** launcher), with an in-game
**MODS** section, a way for mods to add their own garage upgrades, and a few quality-of-life features of its own.
It is a fork of Trinculo54's [FH2 Mod Platform](https://github.com/Trinculo54/fh2-mod-platform) (MIT), brought to
Forza Horizon 3's code and menus.

The platform is one DLL (`DWrite.dll`, also shipped as `d3dcompiler_47.dll`) next to `azimuth.exe`. Azimuth loads it
on start, and it loads every mod DLL in the `fh3-mods` folder.

![The MODS tile on the Home tab, with Exit to Desktop](docs/images/mods-tile-home.jpg)
![The MODS tab: Installed mods, Updates, About, All settings (Get Mods is a placeholder)](docs/images/mods-hub.jpg)
![Installed mods, with each mod's version and description](docs/images/mods-installed.jpg)

## What you get

**For players**

- **MODS in the game's menus.** A MODS tile on the Home tab opens *Installed mods* (switch each mod on or off, open a mod's
  settings), *Updates*, *About* and *All settings* (the *Get Mods* tile is a placeholder and does nothing yet; there is no mod site for FH3). Mods put their own On/Off, slider and choice rows there, in the game's own style.
- **Update checks.** Mods that name their GitHub repo get checked for new releases; the page tells you which one has an update.
- **Custom upgrades in the garage.** Mods can add upgrades to **Garage > Upgrades** (name, stages, prices, icons) that are bought, fitted, stored per
  car and survive Abandon / Modified car prompts like the game's own parts, with a preview on the car in the shop and slider rows on the car's
  Tuning > Springs page.
- **Upgrades stay on your own cars.** The platform keeps track of the car you drive: a car you pick at an event's car select has its upgrades
  from the start, while showcase cars, loaners and AI cars of the same model as yours (one-make races) get none.
- **Speed unit and power unit of their own.** In **Settings > HUD and Gameplay**, under *Units*, two new rows: *Speed unit*
  (Automatic, km/h, mph) and *Power unit* (Automatic, Horsepower, Kilowatts). Metric with km/h *and* horsepower, or Imperial with kW, no longer needs a mod of your own. *Automatic* follows *Units* as before.
- **Sell Car.** In **Garage > My Cars**, a car's action menu gets **SELL CAR** after *Remove car from garage*: a confirmation shows what you get
  (50% of the model's price + 30% of what you paid for its upgrades, both configurable), then the car is removed with the game's own code and the credits are paid.
- **Exit to Desktop** shows the game's own "saving" popup while the game saves, instead of freezing on the garage.
- **No start-up videos**, crash reports that name the mod involved, save snapshots, a log of everything (`fh3-mods\platform.log`).
- Works with [Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions) (its FH3 build) (the platform recognises it and shares the Mods tile and pages), and without it (the platform mounts the `mediaoverride` folder itself).

**For mod makers**

- A C/C++ API (`include/fh3mod.h`, API version 15, documented in [`docs/api.md`](docs/api.md)): game-text edits, game-file overrides, database patches
  (`add_sql`), string-table keys for FH3's hashed tables, Mods-page rows (On/Off, sliders, choices), hooks and byte patches, hotkeys, commands
  (`fh3mod.exe`), interfaces between mods, sessions, save snapshots, the **upgrade bridge** (`register_upgrade`) and tuning rows (`add_tuning_row`).
- A mod is one DLL; [`docs/writing-a-mod.md`](docs/writing-a-mod.md) goes from an empty folder to a release. Mods that change the player's car (upgrades, build-time changes) should read [`docs/car-mods.md`](docs/car-mods.md): the platform tells the player's car from loaners and AI cars of the same model, so mods don't have to. [`examples/hello_mod`](examples/hello_mod/hello_mod.cpp) is the starting point.
- What is known about the game's code: [`docs/fh3-game-notes.md`](docs/fh3-game-notes.md) and [`include/fh3game.h`](include/fh3game.h).

## Requirements

| What | Version |
|---|---|
| Forza Horizon 3 on PC with **Azimuth** | the Xbox One release `forza_durango_release_final.exe` of 5 Jun 2018 (exe timestamp `0x5B16ECCA`) |
| Windows 10/11, or Linux under Wine | the platform needs nothing extra; [Wine notes](docs/linux.md) (written for FH2, not tested on FH3 yet) |

The platform recognises the game by its exe header, so it does nothing (and says so in the log) for any other game or build.
Use it in an Azimuth folder of its own; it is not meant to share one with the FH2 platform (one `DWrite.dll` per folder).

## Installation

1. Close the game.
2. Download the latest zip from the [releases page](../../releases/latest) and copy `DWrite.dll` and `d3dcompiler_47.dll` next to `azimuth.exe`
   (Azimuth 1.0.1 and later load `DWrite.dll`, older ones `d3dcompiler_47.dll`; having both is fine).
3. Make a folder called `fh3-mods` next to them (the platform makes it on first start) and put mod DLLs in it.
4. Start the game. The title of the platform and its version are in `fh3-mods\platform.log`; in the game, **Home > MODS** opens the mods' pages.

Mods built on the platform ship it in their own zips, so most players never need to look for it here.

### Settings of the built-in features

| File (`fh3-mods\`) | Key | Default | Meaning |
|---|---|---|---|
| `platform-sellcar.ini` | `[Sell] Enabled` | `1` | `0` turns Sell Car off. |
| | `[Sell] BasePercent` | `50` | Percent of the model's price you get (0-100). |
| | `[Sell] UpgradePercent` | `30` | Percent of what you paid for the car's upgrades you get (0-100). |
| | `[Sell] CreditReason` | `1` | The reason code the game records with the credit payment. |
| `platform-units.ini` | `[units] Speed`, `Power` | `0` | The two unit rows (0 = automatic); written by the settings rows. |

Restart the game after editing an ini by hand.

## Uninstalling

Delete `DWrite.dll` and `d3dcompiler_47.dll` (and the `fh3-mods` folder). The platform changes no game file on disk: everything
is done in memory and through the `mediaoverride` folder. Cars you sold stay sold.

If you used a mod that adds garage upgrades, remove *that mod* first, keep the platform, start the game once and open the garage:
a save that still names a custom part would crash the game on load without the platform, and the platform repairs such saves when it starts.
Then remove the platform.

## Build

Needs [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases) (clang; the code uses MSVC-style `__try`) and CMake. On Linux or Windows:

```bash
./scripts/build.sh
```

`dist/` then holds `DWrite.dll`, `d3dcompiler_47.dll`, the example mod, `fh3mod.exe` and the headers. The version is `VERSION` in `CMakeLists.txt`;
a release tag (`vX.Y.Z`) must match it. See [`RELEASING.md`](RELEASING.md).

## Credits and thanks

- **[Trinculo54](https://github.com/Trinculo54)**, for the **FH2 Mod Platform** this is a fork of (MIT).
- **[RokkuDayo](https://github.com/RokkuDayo)**, for **[Extra Options](https://github.com/RokkuDayo/ForzaHorizon2_ExtraOptions)**.
- **[MinHook](https://github.com/TsudaKageyu/minhook)** (BSD 2-Clause), the function hooking library.
- **Levilente**, the FH3 port: Mods pages, upgrade bridge, units, Sell Car, saving screen.

## Mods on the platform

| Mod | What it adds |
|---|---|
| [NitroSystem II for FH3](https://github.com/Levilente/NitroSystem-II---FH3) | a Nitrous Kit upgrade and Fast & Furious style nitro (LB) |
| [Universal Wheel Spacers](https://github.com/Levilente/UniversalWheelSpacers_FH3) | front and rear wheel spacers, +1 to +9 cm |
| [Airride](https://github.com/Levilente/AirRide_FH3) | air suspension: lower the car when it stands still |

## License

MIT, see [LICENSE](LICENSE) (the notice of the upstream platform is kept; changes are by Levilente). MinHook is BSD 2-Clause, see `third_party/minhook/LICENSE.txt`.
This is an unofficial fan modification. It is not affiliated with Playground Games, Turn 10, Microsoft or the Azimuth developers.
Forza Horizon 3 is a trademark of Microsoft.
