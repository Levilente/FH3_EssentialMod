# Changelog

## 1.1.1

- Extra Options (and any other DLL next to azimuth.exe) reads the controller too: the minimap follows the right stick, the garage's Manufacturer (View) button works with a DualSense.
- The platform times real use of an XInput pad (shared with the DualSense mod, so the device in use can be told).
- The DualSense mod moved to its own repository, Levilente/FH3_DualSense.

## 1.1.0

- **Every controller for the mods.** Mods and hotkeys read pads with XInput, which only lists Xbox-like controllers, so a DualSense, a DualShock 4 or any other HID gamepad that isn't presented to Windows as an Xbox 360 pad (ViGEm, DS4Windows, Steam Input) did nothing for Nitro, Airride, Launch Control or a mod's hotkey. The platform now reads those controllers itself (Windows.Gaming.Input RawGameController: USB or Bluetooth, no driver) and adds them to `XInputGetState` for the mods only (DLLs in `fh3-mods`, and the platform's own hotkeys): joined to a real XInput pad's buttons, or as pad 1 when XInput has none. The game and every other program get XInput's answer unchanged, so the game never sees a second controller. Sony pads are mapped by their known layout, other gamepads by the usual HID order; the right stick and triggers are learned from the resting axes. No mod needs a change. `fh3-mods\platform.ini` `[input]`: `Enabled=0` turns it all off, `Raw=0` drops the controller reader, `Ignore=054C:0CE6,...` skips devices. `platform.log` names the controllers it found. **The controller reaches the game too**: Azimuth only hands the game Windows' own Gamepad objects (Xbox-like pads), so a DualSense that isn't presented as an Xbox 360 pad did nothing in the game, not even on the landing page. When XInput has no pad of its own, the platform now registers the controller as a gamepad with the engine (the engine's own GamepadAdded handler, found in azimuth.exe by the code that registers it) and takes it away again when the controller goes or a real XInput pad appears. The game's rumble is shared with mods through `Local\\FH3ModPlatform.Vibration`. `[input] Gamepad=0` turns this off. A DualSense (and Edge) is read straight from its HID device, over USB and over Bluetooth, so it keeps working for the mods after something switches it to its full Bluetooth reports (a Windows list of gamepads shows nothing of it then).

## 1.0.0

First release of the **FH3 Mod Platform**, a fork of Trinculo54's FH2 Mod Platform for Forza Horizon 3 (Azimuth), mod API version 15.

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
- **The platform follows the player's car** (API 14): it reads the player's garage from the save, watches the car the player drives and every car the game builds, so for every mod:
  - a car chosen at an event's car select has its custom upgrades from the start;
  - a car that isn't the player's (a showcase car, a loaner, a test drive) gets stage 0 and garage 0;
  - an AI car of the player's model (a one-make race) gets none of the player's upgrades;
  - `upgrade_garage_for_car` / `upgrade_stage_for_car` answer, from a hook in the game's car building, whose car is being built and your stage on it. [docs/car-mods.md](docs/car-mods.md) is the guide for mods that change the car.
- **Tuning rows** (API 13): `add_tuning_row` adds a slider row to the car's Tuning > Springs page, with a header band in the game's style (title, minimum, maximum, unit), per-car visibility, and up to 64 rows (the page scrolls).
- **Shop preview for effects outside the car's data** (API 15): `upgrade_preview_stage` is the stage the upgrade list shows while the cursor stays on it, `upgrade_shop_serial` goes up with every step the list takes.
- **Speed unit and Power unit** rows under *Units* in Settings > HUD and Gameplay: km/h or mph, horsepower or kilowatts, whatever *Units* is set to.
- **Sell Car**: *SELL CAR* after *Remove car from garage* in a car's action menu; pays a share of the model's price and of what was paid for its upgrades (`platform-sellcar.ini`).
- **Exit to Desktop** shows the game's own saving popup.
- The start-up videos are skipped.
- A short `platform.log` by default (start-up, mods, problems); `fh3-mods\platform.ini` `[log] Verbose=1` adds the detailed lines (hooks, file overrides, shop steps, garage reads, car builds, units).
