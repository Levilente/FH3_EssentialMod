# Changelog

## Unreleased

- **Tuning rows (API 13)**: `add_tuning_row` adds a slider to the car's Tuning > Springs page (bound to custom AVUI properties, optional per-car `visible` callback).

## 1.0.0

First release of the **FH3 Mod Platform**, a fork of Trinculo54's FH2 Mod Platform for Forza Horizon 3 (Azimuth), mod API version 12.

- Loads every mod DLL in `fh3-mods`; hooks, byte patches, hotkeys, commands (`fh3mod.exe`), mods calling each other, sessions, save snapshots, crash reports that name the mod involved, a full log.
- **Mods in the game's menus**: a MODS tile on the Home tab with *Installed mods*, *Updates*, *About* and *All settings*; mods add On/Off, slider and choice rows in the game's own style; update checks of mods on GitHub; "Load when playing online" for FH2MP setups.
- **Game files, text and database**: the `mediaoverride` and `mediapc` folders are mounted by the platform itself (or by Extra Options when it is loaded); `add_sql` patches the game's database; `add_string_key` adds strings to FH3's hashed string tables.
- **Upgrade bridge** (`register_upgrade`, API 11; `upgrade_car_model`, API 12): mods add garage upgrades with stages, prices, icons and per-car state, and the platform does the id allocation, shop, purchase and save handling the way the game does it for its own parts:
  - bridge parts are kept out of the game's part set, so they can never overwrite the car's tuning (brake balance, handbrake);
  - the shop's own credit check counts them: not enough Credits gives the game's "You do not have enough Credits" message, and the "install?" prompt shows the whole basket;
  - owned stages (and Stock) go into the basket at 0 CR and replace what was waiting for that slot;
  - shop preview: while the cursor is on a stage, `upgrade_stage` returns it for the car, and a stage in the basket stays on the shop's car until the setup is installed or abandoned;
  - `camera_part` in the upgrade definition points the shop's camera at a part of the car while the upgrade's list is open (the front or rear wheel for wheel spacers, as for rim size);
  - `upgrade_car_model(garage)` returns the model recorded for a garage car, so a mod can tell when the player sits in another car (a mission loaner);
  - the log says which query switched the driven car, and traces basket, commit and abandon steps.
- **Speed unit and Power unit** rows under *Units* in Settings > HUD and Gameplay: km/h or mph, horsepower or kilowatts, whatever *Units* is set to.
- **Sell Car**: *SELL CAR* after *Remove car from garage* in a car's action menu; pays a share of the model's price and of what was paid for its upgrades (`platform-sellcar.ini`).
- **Exit to Desktop** shows the game's own saving popup.
- The start-up videos are skipped.
