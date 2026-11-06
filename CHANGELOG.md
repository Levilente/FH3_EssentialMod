# Changelog

## Unreleased

- **`upgrade_preview_stage(mod, handle)` (API 15)**: the stage of your upgrade the garage's upgrade list is showing right now, for as long as the cursor stays on it (-1 when none, or when the stage is in the basket). `upgrade_stage`'s preview still answers for 300 ms only; nothing existing changed, the new function only adds to the end of the API table.
- **Tuning rows get a header band and a reliable thumb**: each row of `add_tuning_row` now has a band above it like the game's RIDE HEIGHT / LOW / HIGH / cm (title in capitals, the minimum and maximum, the unit). The `label` may be `"Header|Row label (unit)"`; without a bar the header and row share the text, and a trailing `(unit)` goes to the band. The slider's thumb now follows the track slider's value, so it is right after leaving and re-entering the tuning menu (before it kept the old position until moved).
- **The platform follows the player's car itself (API 14)**: it reads the player's garage from the save and watches the car the player drives (the HUD's car), so the driven car, `upgrade_stage(handle, 0)`, `upgrade_active_garage` and the stage callbacks are right for every mod at once: a car switch at an event's car select no longer leaves the chosen car without its custom upgrades until the game saves it, and a car that isn't one of the player's (a loaner) gets stage 0 and garage 0, without code in the mod. Before, the driven garage id came only from the game's statements and lagged behind such a switch by several seconds.
- **AI cars of the player's model get none of the player's upgrades**: the platform watches every car the game builds and tells the player's car from an AI car of the same model (a one-make race), so `upgrade_garage_for_car` / `upgrade_stage_for_car` called while a car is built answer 0 for the AI's. Before, every car of the player's model got the player's wheel spacers.
- `upgrade_garage_for_car(car_id, flags)` and `upgrade_stage_for_car(mod, handle, car_id)` (API 14): which of the player's cars a car the game builds is, and your stage on it, for mods that change a car while it is built (wheel spacers).
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
