# FH3 (Xbox One build) – recon notes for the unified mod platform

Source: `forza_durango_release_final.exe` supplied by the user, 85 425 664 bytes. Everything below was read
from the file on disk (PE headers, strings, RTTI). Nothing here was run in the game. Offsets are RVAs
(add `0x140000000`, the preferred base Azimuth maps it at, as with FH2).

## Identification (for the platform's game detector)

| | FH2 | FH3 (this exe) |
|---|---|---|
| TimeDateStamp | `0x565501ED` (24 Nov 2015) | `0x5B16ECCA` (5 Jun 2018) |
| SizeOfImage | `0x03A18000` | `0x05495000` |
| Preferred base | `0x140000000` | `0x140000000` |
| Entry point | – | `0x17D09E8` |

Game identity from strings: `Microsoft.ForzaHorizon3BlizzardMountain`, `Microsoft.ForzaHorizon3HotWheels`,
`fh3.rtep.msgamestudios.com`, `gameservices.fh3.forzamotorsport.net`. The exe also contains the string
`IDS_TitleFH2Files` (FH2 save/file import), so don't identify the game by the word "FH2" or "FH3" alone –
use the PE header.

Sections: `.text` 0x1000 (size 0x2CD30EC), `.rdata` 0x2CF0000, `.data` 0x42C5000, `.pdata` 0x507A000.
**Unlike FH2, vtables are in `.data`** (writable), not `.rdata`. A vtable scan must cover the whole image.

## Same engine family as FH2 (so most of the FH2 notes carry over in shape, not in offsets)

- Files: `game:\media\...`, `game:\media\zipmanifest.xml`, `ziphashseeds.xml`. Classes `IOSys::CMultiVolume`,
  `CFileVolume`, `CCompressedVolume`, `CachedVolume` – the same volume/mount design the FH2 override folder
  uses. Not yet checked: the resolver vtable slot and the mount/sort slots (FH2: +0x1E8 / +0x1F8).
- Database: `game:\media\Stripped\gamedbRC.slt` and `game:\media\db\gamedb.slt`. Code near `0x18996A1` tests a
  byte at `+0x2EAA` of an object returned by `0x329300` and picks one of the two paths (the first use of the
  stripped path is at `0x18996D1`); presumably a dev/stripped switch, not confirmed.
  The exe also has `keywrapper_gamedb_mackey` / `_decryptionkey` RTTI – the DB may be MAC-checked or
  encrypted; **verify before promising SQL patches**.
- Text: `CStringTableManager`, `CLocString` (FH2's notes name `CLocString` as well).
- Menus: AVUI, XAML under `Anthem\A_*` (`A_Splash`, `A_PauseOptions`, `A_MessageBox`, `A_PCVideoOptions`,
  `A_PopupPCVideoOptions`, `A_VideoOptions`, `A_PauseMenu`). Property bindings by name (`..._EMProperty`
  strings: 1126 of them – the entity-model property system).
- State machine: the exe refers to `game:\media\StateFlow\` (FH2 keeps its state machine in `stateflow.zip`; I
  haven't checked how FH3 stores it).
- Audio FMOD Studio, physics Havok (`HavokModuleEnabled`).
- Exports: 250, **all C++ REST SDK (`web::http`, `web::json`, `uri`) – no FMOD exports** (FH2 exported FMOD by
  name). The FH2 radio mod's lookup-by-export trick won't work here.
- Imports (Xbox One system DLLs that Azimuth translates): `kernelx`, `d3d11_x`, `xg_x`, `EtwPlus`, `AcpHal`, ...

## Vtables (RVA of slot 0; slot count)

| Class | RVA | Slots |
|---|---|---|
| `IOSys::CMultiVolume` | `0x4678750` | 67 |
| `IOSys::CFileVolume` | `0x4678558` | 62 |
| `IOSys::CCompressedVolume` | `0x467A1D8` | 68 |
| `CStringTableManager` | `0x467D858` | 14 |
| `CLocString` | `0x467D8D0` | 12 |
| `Splash` (title screen) | `0x45DC898` | 187 |
| `PauseOptionsMenu` | `0x458D008` | 189 |
| `GameOptionsMenu` | `0x4563CB0` | 189 |
| `PCVideoOptions` | `0x458FF60` | 187 |
| `PopupMessageBox` | `0x45AC778` | 189 |
| `ForzaCar` | `0x42E0650` | 90 |
| `CForzaCommandLineParameters` | `0x42E03A8` | 10 |
| `CUIFreeLookCamera` | `0x4497038` | 30 |
| `RadioStreamFmod` | `0x4638CB8` | 20 |
| `hkWin32FileSystem` | `0x474EB08` | 11 |

(The page classes `Splash`, `PauseOptionsMenu`, `GameOptionsMenu`, `PopupMessageBox` share one base: each has the
same 187–189 slots and the same secondary vtables. I have not compared them with FH2's exe.)

## Entry points worth building in

1. **`cmdline.txt`.** `CForzaCommandLineParameters` builds `"%scmdline.txt"` into a buffer inside the parameters
   object (constructor at `0x1A2C000`, format string at `0x30785C8`; what `%s` is, I haven't traced). The game has a command-line parameter system with a file source. Which switches
   exist is the next thing to read from the parameter registrations. If it works from a file the platform
   can write, it is a zero-hook way to flip dev switches.
2. **Debug channels.** A name table next to the `%scmdline.txt` string lists `DebugConsole`, `DebugMenu`,
   `DebugWebRequest`, `CruiseControl`, `CarPosMetrics`, `MemoryMetrics`, `GameCPUMetrics`, `LoadTimeMetrics`,
   `RenderMetrics`, `PVSMetrics`, `IOMetrics`, `Particles`, `DialogueSystem`... and RTTI `ObjectDebugMenu@RTTI`
   (vtable `0x47341F8`, 1 slot). Probably compiled-out dev tooling, but a metrics overlay or `CruiseControl` is
   worth testing.
3. **Config/data files the game loads loose** (override candidates, same mechanism as FH2's `mediaoverride`):
   `Camera\CameraSettings.ini`, `Camera\CameraPhysics.xml`, `CameraPhysicsSansEffects.xml`,
   `CameraPhysicsSettings.xml`, `CarRelativeCams.xml`, `ThumbnailCamSettings.xml`, `PhotoModeEffectPresets.xml`,
   `GameTunableSettings\*.xml` (AI difficulty, RacingAI, PlayerXPLevels, ...), `GlobalRegistry.xml`,
   `NerdConfig.ini`, `DemoConfigs.xml`, `InputMappingProfiles\`, `ControllerHaptics.xml`, `ControllerRumble.xml`,
   `ControllerRec.ini`, `pcretailtestpackage.txt`.
   The user's FH6 camera port to FH2 (`CameraSettings.ini` + `CameraPhysics.xml`) has a direct FH3 counterpart.
4. **Free-look camera tunables**: `UI/FreeLookCameraSettings/*` (aperture, dynamic focal distance, focal box,
   max blur) – settings keys for a Mods-page slider set.
5. **Options keys** (`Options/...`): CameraAngle, CameraEffects, Rewind, Ghosts, Telemetry, CrossPlay,
   DriftHud, EnableDrivatarLiveries, DamageTireWear, NamesOverCars, ... – the same style as FH2's HUD options.
6. **Track width** exists as a car-model property (`TrackWidthOffset`, `TrackWidthMeters`, `UseFullTrackWidth`);
   tuning columns `Tuning_frontCamber/rearCamber/RideHeight` are in `Career_Garage`. No nitrous symbols at all
   (0 hits for Nitro/NOS) and no sell-car UI strings (only auction-house text).
7. **Online services**: `Services.FH3Release`, plus `Opus`, `Beta`, `Cert`, `Dogfood`, `PreRelease` (and
   `.Trial`) endpoints, all `web::http` through the exported C++ REST SDK (exported, so hookable by name:
   an offline/telemetry switch is a few lines of hooking).
8. **Drivatars**: `ChampionshipDrivatarsState@SaveStates`, `CarMeetDrivatarComponent`
   (`PrepareFreeRoamDrivatars`) – same family as the FH2 WheresMyBuddy notes.
9. **Radio**: `RadioStreamFmod`, `IRadioStream`, `RadioStreamCommon` (no FMOD exports, so the hook site has to
   be found in code).

## Not verified yet (don't assume)

- Whether Azimuth loads the FH3 Durango exe exactly like FH2's (same `0x140000000` mapping, same TLS-helper
  timing) – likely, but only a log from the user's machine proves it.
- All function offsets for FindProperty, the message box, the Settings/MODS page wiring and the Splash button
  handler – they exist as classes here but the FH2 offsets in `game-notes.md` do **not** apply.
- Whether the gamedb is MAC-protected (SQL mods).

## Override folder: mount mechanism (read from code, and the vtable slots checked against the exe on disk)

The file system is `IOSys::CFileSys` (vtable `0x462F340`), derived from `IOSys::CMultiVolume` (`0x4678750`, 67 slots).
Everything the FH2 platform's `mediaoverride` needs maps over:

| What | FH2 | FH3 |
|---|---|---|
| resolver (the function swapped in the vtable) | `0x11F49B0` | `0x1C54DA0` |
| the vtable slot holding it (CFileSys `+0x210`) | `0x2E6EF28` | `0x462F550` |
| game's operator new | `0x03229C0` | `0x1EDBD00` |
| folder volume (`IOSys::CFileVolume`) constructor | `0x11E05A0` | `0x1C40610` |
| folder volume RTTI locator (vtable = +8) | `0x2EAB910` | `0x4678550` |
| folder volume allocation size | `0x50` | `0x88` |
| mount a volume (CFileSys `+0x1E8`) | | `0x1C57F30` |
| sort the volumes (CFileSys `+0x1F8`) | | `0x1C644D0` |
| "does a file exist" (CFileVolume `+0xE8`) | | `0x1C596F0` |

Mount call: `(fs, mount path, IVolume**, priority, sort now, set hash key, hash key)`; the last three are 0 = no
hash-manifest check. The file system's root volume pointer is at `fs+0x68`, zero until the setup at `0x18D3770`
has built it (the platform mounts the folders on the first lookup after it is non-zero). `CFileVolume` has 62
vtable slots.

**Status: implemented in `media_overrides.cpp`, the exe-vs-disk checks pass, not yet run in the game.** The log
line to look for is `media overrides: game:\mediaoverride\ mounted`.

## MODS tab: first anchors (read from code, not run)

The UI is the same AVUI/XAML system as FH2 (pages are `Anthem\A_*`, tiles are `PART_Button_*`, bound by property name).
Found so far (RVAs):

| What | FH3 | FH2 counterpart in settings.cpp |
|---|---|---|
| wire a named tile button on a page `(page, const char* name, Pmf*)` | `0x1687960` (82 callers) | `RVA_WIRE_BUTTON 0xD9FC10` |
| Options page: wires `PART_Button_Controls/Video/Hud/Audio` | `0x14DACB0` | `RVA_PAUSE_WIRE_TILES 0xDC8A60` |
| Pause menu: wires Extras / CarPacks / Achievements / ... tiles | `0x1585690` | `RVA_PAUSE_MENU_WIRE 0xDC6E30` |
| (a second tile wiring function, same helper) | `0x1587030` | |
| scene record: HudOptions / PauseOptions / PauseMenu | `0x1182CB0` / `0x1189F70` / `0x1189C10` | `RVA_HUD_SCENE_RECORD 0xB3A3E0` |
| code binding `Options/EnableDrivatarLiveries` (HUD page model) | `0x14DF5A0`, `0x14FD660`, `0x14FD740` | `RVA_MODEL_PROPERTY 0xD58660` |

Not yet found: FindProperty, the AVUI string make/free, the box helpers, the popup/message box functions, the
Splash start handler, the UI sound objects, the focus helpers. Each needs the same "find by string, then by shape" pass.

## UI is plain XAML in UI.zip (read from the game files and the exe)

- `media\UI.zip` holds `Scenes\Anthem\A_*.xaml` (pause menu, options pages, splash, ...). A zip's root entries appear under a folder
  named like the zip (`Camera.zip` -> `game:\media\Camera\...`), so a loose override would be `UI\Scenes\Anthem\<name>.xaml`.
- `A_PauseMenu.xaml` lays the tiles out on a grid (`TileMenuNavigationManager.GridX/GridY`, style `A_Style_Button_Tile_2x2` / `2x1` /
  `6x2`); tiles are bound by `x:Name="PART_Button_*"`. Home tab tiles: `PART_Button_HorizonOnline` / `HorizonSolo` (0,2 2x2),
  `PART_Button_HomeFeatured` (2,2 6x2), `GameParty` (8,0), `Photomode` (10,2), `Dronemode` (10,3), `ExitToDesktop` (10,1 2x1) and
  `ExitToDesktop_SinglePlayerFreeRoam` (10,0 2x2), both `TileState="Disabled"` in the file (the game's code sets the state).
- `ExitToDesktopCommand` is a property on the page, descriptor built at `0x15AFB90`.
- Scene loader: `0x1EEF1C0` builds `game:` + `Scenes\%s.xaml` and hands the path to AVUI's load (`0x1ED1160`, `0x12144E0`); where the
  bytes are read is not found yet (needed to patch XAML in memory instead of shipping a file).
- `CFileVolume` constructor (`0x1C40610`) stores the folder string at `+0x28`; `+0x80` is 1 when the first path component has no `:`.
  Whether a volume given an absolute drive path works is not known.

## Navigation is data-driven (StateFlow), found while tracing the Horizon Group tile

- `media\stateflow.zip` (-> `game:\media\stateflow\`) holds the state machine as XML: `ui_screens.xml` defines `<state id="show_*">` with a
  `<behavior id="UIPage...">` per page class and its `<transition id=...>`s; `pause.xml` has the nodes (`pause_menu` = `ui_screens.show_pause_menu_tiled`
  with `<exit id="online_party_list" target="online_party_list" />` etc.). Pause > Settings is `UIPagePauseOptionsMenu`, whose `hud` transition leads to
  `show_hud_options` (`UIPageHudOptions`).
- The Home tab's Horizon Group tile is `PART_Button_GameParty` (name pointer in the global at `0x45866C0`); its click handler is `0x14157AFD0`
  (wired by `0x1585690` through `0x1553850(page, name, &pmf)`). When the tile is enabled it presses the named button (`0x16A9A80`); otherwise it fires a
  transition with `0x1689F60(page, &transition_id, 0)`, ids being static objects (`0x144E31B18`, `0x144E32720`). The tile's enabled state is set in code
  (`0x159BBD0` uses the name) - unlike `PART_Button_ExitToDesktop`, which stayed enabled from the XAML.
- Consequence for the Mods page: the navigation part can be a state-flow overlay (an `<exit>` on `pause_menu` to a node showing an existing page state),
  no new code offsets; what still needs code is (1) firing/enabling from the tile, (2) the page's XAML path (scene record builder `0x1182CB0` for HudOptions)
  and (3) property bindings (FindProperty, not found yet).

## MODS tile, first version (untested on hardware)

- Tile enabled + renamed in `A_PauseMenu.xaml` (`PART_Button_GameParty` TileState, data item `A_MenuDataItem_GameParty` Heading="MODS").
- `pause.xml` overlay: node `online_party_list` -> `ui_screens.show_placeholder_text_scene text="FH3 MOD PLATFORM" wait_for_button_press="true"`, default exit back to
  `pause_menu.enterback`. (`UIPagePlaceholderTextScene` shows the sticky `PlaceholderTextSceneText` in A_PlaceholderTextScene.xaml; A closes it.)
- Handlers `0x157AFD0` (page) and `0x157B060` (page, int) are hooked by `mods_page.cpp` and just call press `0x16A9A80(page, *(char**)0x45866C0)`.
- Overlay now covers any zip: `<zip name>\<entry>` under `mediaplatform\` (UI.zip -> UI\, stateflow.zip -> stateflow\).
- Unverified: that stateflow is read through the mounted file system (so the overlay wins), and that pressing a disabled tile's button still fires.

## Findings from the first hardware tests (1 Nov)
- Tile state setters: `0x16A4CF0(page, name, state)` and `0x1695B90(page, name, value)`. `0x159BBD0` first sets every tile to 0 then enables by condition with 1 -> **0 = Disabled, 1 = Enabled**.
  Setter B also gets 1 and 3 for the Horizon Group tile (3 = locked, online services missing). First attempt forced 0 (wrong way round).
- `ExitToDesktopCommand` getter `0x15AFB60` returns page+0x4F8: an object with vtable game+0x44CDBD8; slot 8 (`0x11CA720`) = has delegate (`[this+0x40] != 0`),
  slot 9 (`0x11CCA00`) = run it (calls the std::function at +0x40). The delegate is empty in this build, so the tile does nothing; the platform hooks both for that object.
- `0x15F31E0(obj, reason)` is a message helper used by the other tile click handlers (reason 12); it wasn't what showed the Feature Locked popup for the Horizon Group tile.
- Correction: `0x16A9A80(page, name)` shows the named tile's Feature Locked popup (reason from the tile, switch in `0x168ADD0`; "temporarily locked" = initial experience).
  `0x1694B80` = is the tile locked. The Horizon Group handler's real path: `0x1689F60(page, &TransitionId, 0)` with `0x4E31B18` = "online_party_list".
- Tile state enum (setter A / vtable +0x5E0): 0 shown, 1 hidden, 2 locked. Setter B (`0x1695B90`) = lock with a reason (calls +0x5E0 with 2).
- Click handlers: `0x1553850(page, name, &{fn, adjust})`, called from the wiring `0x1585690`; Exit tile name pointer at `0x4586788`.
- Intro video: entry 4 of the start-up table built by `0x1728DC0` (T10_MS_Combined.bk2), length constant `mov rax, 21.0` at `0x1729326`; set to 0 in memory - confirmed working.

## Message box and property lookup (for the Mods page)
- Message box, as the pause menu builds its Yes/No popups (Tombola spin, `0x157B630`): `box = 0x13FDEE0(page)`; text: `0x1CA7BC0(&holder, table, id)` (table name
  char*, id = int from the string-id globals; holder -> ref-counted string, +0x10 AddRef / +0x18 Release), then `0x1617570(box, &holder)` title / `0x1616EE0` body;
  `0x1598950(box, preset)` buttons (0 OK, 1 OK/Cancel, 2 OK/Back, 3 Back, 4 Yes/No, 5 Select, 6 Select/Cancel, 7 Finish/Back/Continue);
  `0x1598510(box, button, &std::function<void()>, show)` = what a button does, and shows the box. Exit texts: table "Main" (`0x2D4F6B0`),
  ids `0x4E57588` IDS_ExitGame_Title / `0x4E5758C` IDS_ExitGame_Desc. String tables: `media\Stripped\StringTables\<lang>.zip` (*.str, UTF-16 texts).
- String-id globals are filled by static initializers `lea rcx,"IDS_..."; call 0x1CABB60; mov [global],eax` (scanned into a map).
- **FindProperty: `0x200C1D0(type, name)`** - walks `type+0x40` (list of {descriptor, next}), compares `descriptor+0x40` (char* name) with `name+0x28`
  via `0x209A080`, then recurses into the base type at `type+0x80`. Descriptor layout matches the FH2 one (+0x28 getter, +0x40 name), e.g. ExitToDesktopCommand's
  static descriptor at `0x4E36D10` (vtable `0x42F99B0`, getter `0x1575E30`). Called from the binding code (`0x1FC70F0`, "Unable to find property: {0}.").
- Save: `SaveProfile` behavior (`0xD80B80`): game object `*0x4D6FC00` -> vt+0x48 -> vt+0xE8 = saver; `0x3343B0(saver, blocking, force)` (`0x334390` blocking, `0x3343A0` async).
- TransitionId ctor `0x155E6D0(this, const char*)` (vtable, std::string name at +0x10). A new transition needs: an `<exit>` + `<transition>` in ui_screens.xml
  `show_pause_menu_tiled` and an `<exit>` + node in pause.xml.
- Placeholder page `UIPagePlaceholderTextScene` (A_PlaceholderTextScene.xaml, Text = sticky `PlaceholderTextSceneText`); DataTrigger works in AVUI XAML.
- Wait popup classes exist (`PopupWaitMessageBox`, ctor `0x1600A90`) but are created by a page factory, not the state flow.

## NitroSystem II port (started 1 Nov) - what it needs from the game
Source: github.com/Levilente/NitroSystem-II (FH2, nitro_mod.cpp + nu_upgrade.h/nu_sql.h). FH2 exe for comparison: azimuth-windows\Games\FH2\...\forza_durango_release_final.exe.
FH2 hooks to find again in FH3:
1. `CCarDynamics::UpdateDynamics` (FH2 0x12C1EA0, CCarDynamics vtable slot 16 of 21; body 0x12C1F20/0x12BA060). Offsets: +0x40 forward, +0x50 velocity, +0x178 == 1 simulated.
   FH3's CCarDynamics vtable (0x4680EA0) has 17 slots and a different layout (fields at +0x1735, +0xD01C, second vtable at +0x4B0 with 474 slots) - no 1:1 slot match.
2. `CWorldCollObjMan::Update` (FH2 0x384C00), player position at mgr+0x4F0.
3. `CarPresentation11` backfire: entry 0x13B0D70 (vtable slot 76), call 0x13B0DE6 - exhaust flames.
4. Logic thread frame step 0xFA78A0.
5. Cameras: view matrix slot +0x50 (0x1034360 mounted / 0x1034430 chase), FOV 0x10340F0 / 0x1034120, six camera vtables.
6. `HUDStatusSpeedometer` tick 0xAD6070 (+ property getters 0xADD560/0xADD620, raise 0xAD1A80) - NOS gauge; HUD XAML/swatchbin textures.
7. Upgrade (Nitrous Kit stages): game database rows via the platform's SQL service; per-car stage storage.
8. Settings on the MODS page (platform settings API: sliders, toggles).
