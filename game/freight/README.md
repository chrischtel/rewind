# Spedition

*(working title)*

A small freight company you inherited, run from a depot on a branch line.
This is a game, not part of the engine: everything under `engine/` stays
generic, and this folder is one of its users.

Grandfather left you a yard at the edge of a valley town: a siding off the
branch line, two lorries, a shunter that starts on the third try, a filling
station at the gate, and more debt than the whole lot is worth. Goods need
moving. Rail is cheap by the tonne and needs a siding at both ends and a
slot on the line; road goes anywhere and eats money. Every job is the same
question asked differently, and the answer changes when the price of diesel
does.

The valley has gradients, and gradients are the game. A loco pulls what its
tractive effort allows against the mass behind it and the grade under it.
Load one wagon too many and the train stalls on the 1.8% out of Oberdorf
and you lose the afternoon. That number is real, it is calculable, and the
map is interesting because of it.

## The three documents

| File | What it holds |
| --- | --- |
| `README.md` | this: the pitch, the parts, and the roadmap |
| [DESIGN.md](DESIGN.md) | the simulation: vehicles, cargo, contracts, money, time |
| [STYLE.md](STYLE.md) | the look: palette, type, motion, spacing, sound |

## The rule about the engine

When the game needs something the engine does not have, **it goes in the
engine, generic, or it does not get built.** No game-specific hooks in
`engine/`, and no working around a missing engine feature inside
`game/freight/`. The traffic game was built this way and it is why the
network, the agents and the curve tools exist at all.

What this game will ask the engine for is listed under *Engine additions*
below, and each one lands as a feature any game could use.

## Run it

```bash
cmake --build --preset vs2026-release
```

```bash
./build/bin/Release/freight.exe
```

| Input | Does |
| --- | --- |
| Dispatch | sends that vehicle out empty, loads it, brings it back, banks it |
| WASD / arrows | pan |
| wheel | zoom |
| F4 / F2 / F5 / F1 | rewind, pause, replay and verify, overlay |

`--zoom 2` starts closer in. The engine's own `--frames`, `--speed`,
`--screenshot` and `--command 1:0@20` (dispatch vehicle 0 at frame 20) work
here too, which is how this folder's screenshots get made.

**The fonts are not in the repository.** Drop `IBMPlexSans-Regular.ttf` and
`IBMPlexMono-Regular.ttf` into `assets/fonts/` and the game picks them up on
the next run; without them it falls back to the engine's built-in face and
logs two lines saying so. They are free under the OFL, but which files land
in the repository is the author's call.

## What is in it

Phase 1 runs: the yard, the branch, the road, two vehicles, a standing job
and a balance that moves. Everything below that is not marked *(phase 1)* is
still the plan.

**The line, running.** *(phase 1)* A branch from the yard up to the quarry
above Oberdorf, a road taking the long way round the same hill, a lorry and a
locomotive with four hoppers. Dispatch one and it runs out empty, loads,
comes back, unloads, and banks the difference between what the gravel paid
and what the kilometres cost. Rail clears about twenty times what the lorry
does per trip and takes about six times as long over it, which is the whole
argument of the game in miniature.

**A day that costs money.** *(phase 1)* A calendar from `sim/clock.h` running
at ninety simulated seconds to the game hour. Every midnight the standing
charges and grandfather's interest come off, and yesterday's result appears
in the top bar.

**One customer.** *(phase 1)* The valley takes 260 tonnes a day at the full
rate and 40% for anything past it, so there is a point in the day where
running the fleet harder stops being worth the diesel. A placeholder, but one
with the right shape: contracts replace it in phase 3.

**The yard.** A depot with a siding, a loading ramp, a workshop, a fuel
tank and a filling station on the road side. You lay track and hardstanding
yourself with the network tool, and the buildings go where there is room
for them.

**The line.** A single-track branch with passing loops, joining a main line
at the valley mouth. Slots on the main line are bought, not owned, so a
late departure is a missed slot and a missed slot is a missed day.

**The fleet.** Lorries and rail wagons with a mass, a capacity and a
condition per component. A loco with a tractive effort curve. Nothing has
hit points; things wear where they are used, and a brake worn on a long
descent fails on the next one.

**The drivers.** Hours, rest, and a wage. One driver is one shift is one
constraint, and the whole schedule bends around them.

**The contracts.** Move so many tonnes of something from here to there by
a date, for a price. Some recur weekly, some are one-off, some are bid for
against a competitor you never see except in the price.

**The filling station.** Diesel bought wholesale into your own tank, drawn
by your own fleet, and sold at the pump to passing traffic at a price you
set. It is the second income, the reason the yard is a place, and the
warmest corner of a game about tonnage.

**The record.** Every run is on the timeline. A delivery that came in late
can be rewound and watched, and the hour it lost is visible rather than
guessed at.

## Where the pieces come from

| Engine | Used for |
| --- | --- |
| `sim/network.h` | one network for the track, one for the road; nodes carry elevation, so grade comes for free |
| `sim/network_agents.h` | trains and lorries: following, spacing, passing loops, signals |
| `sim/graph.h` | routing with dynamic costs; `RouteCache` for a fleet re-planning at once |
| `project/data_table.h` | vehicles, cargo, contracts, buildings and fuel prices as JSON rows, hot reloaded |
| `render/ui.h` | the shell: contracts, fleet, ledger, the pump price |
| `render/mesh.h` | track, hardstanding, buildings and vehicles, all geometry, no art |
| `core/curve.h` | drawing track, and the loco power curve |
| `sim/savegame.h` | the company, saved and versioned |
| `sim/timeline.h` | rewinding a run that went wrong |

## Roadmap

One list at a time. Each phase ends with something that runs.

### 1. The line runs — done

A yard, a branch to one town, a lorry and a train carrying one cargo and
getting paid for it. Money goes up and it goes down. The style rules from
[STYLE.md](STYLE.md) applied from the first commit: nine colours, the 8 unit
grid, real metres in the world, and the money figure counted rather than cut.
The one rule not yet met is the font, which is not in the repository (see
*Run it* above).

Two decisions worth knowing about, both written up in
[DESIGN.md](DESIGN.md): rail and road are separate networks, because the
engine cannot yet price an edge differently for different vehicles; and the
drawn valley is one twelfth of the railway it stands for.

*Engine: `core/ease.h` (easing curves, `approach`, `Tweened<T>`) and
`sim/clock.h` (civil dates, `Clock`, `ClockEdge`). Both header-only, both
covered by `tests/ease_test.cpp` and `tests/clock_test.cpp`.*

### 2. The physical run

Mass, grade, tractive effort, rolling and grade resistance, braking. Fuel
burnt for work done rather than distance covered. Wear accumulated per
component by the work that causes it. Driver hours and rest.

*Engine: `grade_at()` on `CurveNetwork`, a resistance hook on
`NetworkAgents`.*

### 3. The company

Contracts with deadlines and penalties. The ledger: fuel, wages,
maintenance, rent, interest. A monthly close. Reputation that opens bigger
work. The loan grandfather signed.

*Engine: UI II — scrolling lists, tables with columns, tabs, number fields;
currency, date and unit formatting in `core/localization.h`.*

### 4. The filling station

Wholesale diesel into a tank with a capacity, drawn by the fleet and sold
at the pump. A price you set against a volume that responds to it. Passing
trade with a rhythm to the day.

### 5. The look

The expensive half of [STYLE.md](STYLE.md): 2D lighting, a soft shadow
pass, colour grading and vignette, weather on the yard, and the sound —
which carries more of "hochwertig" than any of the pixels do.

*Engine: 2D lighting and post-processing (already on the engine's own
later list).*

### 6. The valley

A generated region: a branch line, a main line at the mouth, six to eight
towns with industries that want things moved. Terrain that makes the
gradients, and the gradients that make the routes.

### 7. Building the yard

Sidings, ramps, hardstanding, warehouse, workshop, tanks. Placement with
the editor tool API, costing money and taking time to build.

## Scope, deliberately

Solo, and the programming is the strong half. So:

- **No hand-drawn art, anywhere.** Every surface is geometry and a shader.
  A lorry from above is a shape with the right proportions.
- **No hand-built map.** One valley, generated, seeded.
- **No characters.** Drivers are names, hours and a wage.
- **All content is data.** Vehicles, cargo, contracts and buildings are
  rows in JSON tables that hot reload. Adding a lorry is a row, not a
  sprite sheet.

The depth is in the simulation, which is code, and the polish is in
restraint, which is also code.
