# Changelog

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
