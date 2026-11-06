# Changelog

## 1.0.10

- Shop preview: a stage put in the basket stays on the shop's car (`upgrade_stage` returns it) until the setup is installed or abandoned, or another car is driven, like the game's own basket parts. Before, backing out of the list after adding it showed the fitted stage again.

## 1.0.9

- Shop preview: `upgrade_stage` answers with the previewed stage only for 300 ms after the cursor moved onto it, which covers the car rebuild the shop starts then. Backing out of the list rebuilds the car with no call the platform sees (the game's part set never changed for a bridge part), so 1.0.8 still left the preview on the car; now that rebuild gets the fitted stage.

## 1.0.8

- Shop preview: the previewed stage no longer stays on the car after backing out of the list or leaving the garage. The shop puts a setup on its car (`game+0x38BCD0`) when it enters or leaves a list and before each cursor move; the preview ends there, and the show step sets it again while the cursor is on a stage.

## 1.0.7

- **Upgrade bridge: owned stages go into the basket at 0 CR, like the game's own owned parts.** Choosing a stage you already own (or Stock) used to skip the basket, so what was waiting for that slot (say Stage 3) stayed in it and Install Parts did nothing. The stage now replaces the waiting one in the basket and is fitted at the install without a charge.
- Basket, commit and abandon steps are logged (first 100-200 events) to make shop problems easier to trace.

## 1.0.6

- **Upgrade bridge: the game's own credit check now counts bridge upgrades.** The shop's affordability check (`0x16CA9F0`, total cost against Credits) summed only the game's parts, so a mod upgrade you couldn't pay for slipped through. The platform adds the price of the bridge stages in the basket to that total, so the game itself says "You do not have enough Credits" and shows the right total in the "install?" prompt: one check for everything in the basket. The message box from 1.0.5 stays only as a fallback.

## 1.0.5

- **Upgrade bridge: not enough Credits is said.** The game's own affordability check does not count the price of a bridge upgrade, so installing one you couldn't pay for did nothing at all, silently. The platform now shows a message box ("INSUFFICIENT CR!", the price and your Credits) after the commit and leaves the upgrade uninstalled and unpaid. Other parts in the same basket are handled by the game as before.

## 1.0.4

- API 12: `upgrade_car_model(garage)` returns the model recorded for a garage car, so a mod can tell when the player is in another car than the garage car it believes is driven (mission loaners).
- The log says which query switched the driven car (`upgrades: driven car is garage N (was M, why)`).

## 1.0.3

- Shop preview: the preview ends when the shop puts the fitted part back (backing out of the list) or moves on to one of the game's own parts, not only on purchase, Abandon or another car. Car rebuilds are logged with their caller (diagnostics).

## 1.0.2

- **Upgrade bridge: shop preview.** While a stage of a bridge upgrade is looked at in the shop, `upgrade_stage` returns that stage for the car (the game's "show this part" step, `game+0x38D0B0`, is hooked), and the shop rebuilds the car as it does for its own parts. A mod that builds the car from `upgrade_stage` shows the stage on hover. The preview ends with the purchase, when the shop is left without buying, or when another car is driven.

## 1.0.1

- **Upgrade bridge: tuning no longer gets overwritten.** The game's part setter (`game+0x36B7A0`) stores a part at `part set + 8 + 4*enum` with no bounds check, and the car's tuning setup sits right after the 0xB4-byte part set. Choosing a stage of a bridge upgrade (enum 47+) in the shop wrote its part id into the tuning: the Nitrous Kit (enum 47) could move the brake balance all the way to the rear, and Universal Wheel Spacers (48/49) took the handbrake away until the next start. The platform now keeps parts of enum 44+ out of the game's part set (the bridge keeps them in `platform-upgrades.ini`) and logs the first such calls.
- `units.cpp` builds with newer llvm-mingw (SEH moved into helpers without C++ objects).

## 1.0.0

First release of the **FH3 Mod Platform**, a fork of Trinculo54's FH2 Mod Platform for Forza Horizon 3 (Azimuth), mod API version 11.

- Loads every mod DLL in `fh3-mods`; hooks, byte patches, hotkeys, commands (`fh3mod.exe`), mods calling each other, sessions, save snapshots, crash reports that name the mod involved, a full log.
- **Mods in the game's menus**: a MODS tile on the Home tab with *Installed mods*, *Updates*, *About* and *All settings*; mods add On/Off, slider and choice rows in the game's own style; update checks of mods on GitHub; "Load when playing online" for FH2MP setups.
- **Game files, text and database**: the `mediaoverride` and `mediapc` folders are mounted by the platform itself (or by Extra Options when it is loaded); `add_sql` patches the game's database; `add_string_key` adds strings to FH3's hashed string tables.
- **Upgrade bridge** (`register_upgrade`, API 11): mods add garage upgrades with stages, prices, icons and per-car state, and the platform does the id allocation, shop, purchase and save handling. NitroSystem II's Nitrous Kit is the first mod on it.
- **Speed unit and Power unit** rows under *Units* in Settings > HUD and Gameplay: km/h or mph, horsepower or kilowatts, whatever *Units* is set to.
- **Sell Car**: *SELL CAR* after *Remove car from garage* in a car's action menu; pays a share of the model's price and of what was paid for its upgrades (`platform-sellcar.ini`).
- **Exit to Desktop** shows the game's own saving popup.
- The start-up videos are skipped.
