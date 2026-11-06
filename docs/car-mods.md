# Mods that change the player's car

This page is for mods that give the player's car something it doesn't have in the game: a garage upgrade (nitro, wheel spacers, air suspension), a change to how the car is built, or an effect while it is driven. Everything here needs platform API 14 (platform 1.0.0 and newer); check `api->api_version >= 14` and keep a fallback if you support older platforms.

The short version: **let the platform decide which car is the player's.** Don't track the driven car yourself, don't compare car models yourself, and don't keep your own idea of "loaner" or "AI car". The platform already watches the game for all of that, and every mod that asks it gets the same answer.

## What the platform does for you

| Situation | What the platform does |
|---|---|
| The player gets into another garage car (garage, an event's car select) | The driven garage car changes at once: `upgrade_active_garage()` returns the new garage id, `upgrade_stage(mod, handle, 0)` the new car's stage, and your `on_stage` callback runs. |
| The game hands the player a car that isn't theirs (a showcase event, a mission loaner, a test drive) | `upgrade_active_garage()` returns 0, `upgrade_stage(mod, handle, 0)` returns 0, and `on_stage` runs with garage 0, stage 0. When the player is back in their own car, it all comes back. |
| The game builds an AI car of the same model as the player's (a one-make race, traffic) | `upgrade_stage_for_car` and `upgrade_garage_for_car` called while that car is built return 0: the AI car gets none of the player's upgrades. |
| The player is in the upgrade shop | `upgrade_stage` returns the stage under the cursor (preview) for a moment while the game rebuilds the shop car, and a stage in the basket until it is installed or abandoned. |

How it knows: it reads the player's garage from the save (garage id and car of every car the player owns), watches the statements the game runs when it puts the player into a garage car, follows the car the HUD shows (the car the player really drives), and watches every car the game builds (`game+0x189BCC0`, the in-Car build that fills the car's spec at `Car+0x140`). A car of the player's model that is built while the player's own car of that model is already on the road is someone else's.

## Pick the right kind of mod

### 1. An upgrade with an effect while driving (nitro, a HUD, a sound)

Use the upgrade bridge for the upgrade and ask for the stage of the car being driven:

```cpp
static int g_kit = -1;

static void on_stage(void*, uint32_t garage, int stage) {
    // garage 0 = the player isn't in one of their own cars: no effect
    g_level = garage ? stage : 0;
}

FH3MOD_EXPORT int fh3mod_init(const FH3ModApi* api) {
    // ... register_mod ...
    static const FH3UpgradeStage stages[] = {
        {L"Stock", L"No kit", 0, nullptr, nullptr},
        {L"Stage 1", L"A small kit", 3000, nullptr, nullptr},
    };
    FH3UpgradeDef def{};
    def.size        = sizeof def;
    def.id          = "mymod.kit";
    def.name        = L"My Kit";
    def.description = L"What it does";
    def.area        = 1;   // Engine
    def.stage_count = 2;
    def.stages      = stages;
    def.on_stage    = on_stage;
    def.preferred_slot = -1;
    g_kit = api->register_upgrade(g_mod, &def);
    return 1;
}
```

Use the stage from `on_stage` (or `upgrade_stage(mod, handle, 0)` every so often). That is all: car switches, loaners and the shop are already in that value. You don't need `upgrade_car_model` or your own loaner check any more; NitroSystem II still has one from before API 14, and it agrees with the platform.

### 2. An upgrade that changes how the car is built (wheel spacers, a track, a weight, a ride height)

The values a car is built from are read once, when the game builds it; changing them later does nothing (tried: the car's spec at `Car+0x140` is still there while it is driven, but the wheels don't move). So the change has to happen in the build, and for every build you have to know whose car it is.

1. Hook the place where the game reads or stores your value in the car loader, after `wait_for_code`. The central car loader is `game+0x19E6A80`; Universal Wheel Spacers hooks the instruction right after the track values are stored (`game+0x19E9027`, `hook_instruction`), where `r14` is the spec and `[rsp+0x68]` holds the car's Data_Car id.
2. In the hook, ask the platform for your stage on that car:

```cpp
void on_spec_built(FH3Context* ctx, void*) {
    const int car_id = *reinterpret_cast<const int*>(ctx->rsp + 0x68);   // Data_Car id, e.g. 2154 or 445
    const int stage  = g_api->upgrade_stage_for_car(g_mod, g_handle, car_id);
    if (stage <= 0) return;   // not the player's car (AI, loaner), or stock
    // change the value in the spec by the stage
}
```

`upgrade_stage_for_car` answers for the car being built on this thread right now: the player's car of that model (the shop's preview stage while the cursor is on one of yours), 0 for an AI car of the same model and for cars the player doesn't own. If you need the garage id too, call `upgrade_garage_for_car(car_id, FH3MOD_CAR_MAKE_DRIVEN)` and then `upgrade_stage(mod, handle, garage)`.

3. Call it from the hook itself, while the game is building the car. Called from another thread, or later, the platform can't tell the player's car from an AI car of the same model.
4. Don't remember the answer between builds. The shop rebuilds the car on every cursor move, and the answer changes with it.

### 3. An effect on the car while it is driven (not an upgrade)

Use `upgrade_active_garage()`: 0 means the player is in a car that isn't theirs. If you need the car object itself, the HUD speedometer controller (`game+0xB26690`, the player's Car at `+0x120`) is what the platform and NitroSystem II use; hook it with `hook_function` (the platform's own hook shares the chain).

## Things not to do

- Don't decide which car is driven from the game's database statements yourself (`UPDATE ... TopSpeed`, `WHERE Id=`): the game saves the stats of other cars too, and browsing the garage reads every car.
- Don't compare the built car's model with the driven car's model to decide. That was the old way; it gave the player's upgrades to every AI car of the same model.
- Don't change the car after it is built and expect the car to follow.
- Don't write your part into the game's part set or tuning (enums 0 to 42 are the game's). The upgrade bridge keeps its parts out of it.
- Don't block in `on_stage`; it runs on the game thread.

## What the platform can't tell apart

A car that isn't the player's but is the same model as one of their cars, built while the player is getting into that car of theirs (the moment between a car switch and the player's new car being built), is taken for the player's. In practice that means a loaner of exactly the model the player just chose. Everything else (one-make races, traffic, showcase cars, test drives) is told apart.

## Testing checklist

Turn on your mod's debug log and keep `fh3-mods\platform.log` open. The platform's lines start with `upgrades:`.

- [ ] Shop: move the cursor over your stages (the car shows each one), back out with B (the car goes back to the fitted stage), buy one, install the setup, leave the garage.
- [ ] Free roam after the shop: the installed stage is on the car.
- [ ] An event whose car select makes you switch cars: the chosen car has its upgrades from the start (`upgrades: car ... is built: the player's`).
- [ ] A one-make race in a car with your upgrade: the AI cars don't have it (`someone else's ... the player's car of that model is on the road already`).
- [ ] A showcase event or loaner: no upgrade on that car (`isn't one of the player's cars`), and back in your car it returns.
- [ ] Restart an event, quit an event, fast travel, a new session: still right.

Useful platform log lines:

| Line | Meaning |
|---|---|
| `upgrades: the player's garage has N cars` | the garage was read from the save |
| `upgrades: driven car is garage N (was M, why)` | the driven garage car changed |
| `upgrades: car X (model M) is built: the player's / someone else's (why)` | the decision for a car build |
| `upgrades: the driven car (Data_Car N) isn't one of the player's cars` | a loaner or showcase car |
| `upgrades: the player drives Data_Car N, garage car G` | what the HUD shows |
