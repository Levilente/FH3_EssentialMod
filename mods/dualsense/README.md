# DualSense for Forza Horizon 3

A mod for the FH3 Mod Platform (1.1.0 or newer). Copy `DualSense.dll` into `fh3-mods`. A DualSense or DualSense Edge on **USB** then feels the car:

- **Light bar**: blue at low revs, towards red at the redline, flashing on the rev limiter. Player lights count the revs.
- **L2** is a brake pedal (progressive resistance), **R2** vibrates on the rev limiter. Both are off outside driving.
- **Rumble**: the engine's hum at its revs, a knock at every gear change, a buzz on the limiter, a thump on impacts and landings.
- Rows on the MODS page: effects on/off, rumble strength, adaptive triggers and their strength, lights, rev counter. Also in `fh3-mods\DualSense.ini` (`ColorR/G/B` sets the light bar's base colour, `Log=1` logs more).

Nothing is written to the game, and the controller's input is not touched: the game and the other mods get the pad as before (the platform makes any controller readable for mods). Close Steam's PlayStation controller support and DS4Windows while playing, or both will drive the lights and rumble. Bluetooth isn't supported (the controller would have to switch to its full reports, which changes the input the game reads). When the game ends or another window is in front, the controller goes back to itself.

Not in it yet (the FH2-Input mod has them, they need FH3 data this mod doesn't have): HD haptics for the road surface, ABS and wheel-spin trigger effects, menu haptics, the game's own rumble.

Build: `cmake -S mods/dualsense -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake && cmake --build build`.

The HID report layouts and trigger effect encodings follow the Linux `hid-playstation` driver and Nielk1's DualSense trigger notes (as FH2-Input uses them).
