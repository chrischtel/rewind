# Traffic

A top-down traffic management sim built on the Rewind engine. This is a
game, not part of the engine: everything under `engine/` stays generic, and
this folder is one of its users.

Drag with the left mouse button to draw a road. The stroke is fitted to a
curve and becomes a road with the lanes of the selected type; start or end
the stroke on a junction to connect to it. Cars appear on their own, plan a
route over the network, follow the car in front, queue at junctions and give
way to whoever is already crossing. Bulldoze removes a road or a junction,
and the cars that were on it are put back on the nearest lane and re-plan.

The whole city is simulation state on the fixed step, the camera included,
so F4 rewinds the traffic, F5 replays it and verifies every tick, and the
inspector can edit any car mid-jam.

## Run it

```bash
cmake --build --preset vs2026-release
./build/bin/Release/traffic.exe
```

| Input | Does |
| --- | --- |
| left drag | draw a road (Bulldoze: click to remove) |
| right drag | pan, and cancel the road being drawn |
| wheel | zoom around the cursor |
| WASD / arrows | pan |
| F4 / F2 / F5 / F1 | rewind, pause, replay and verify, overlay |

Arguments beyond the engine's own (`--frames`, `--screenshot`, `--speed`):
`--zoom 6` starts closer, `--spawn 0.3` sends a car every 0.3 seconds.

Hot reload while it runs:

```bash
./build/bin/Release/rewind_run.exe build/bin/traffic_hot.dll
```

Edit `traffic.cpp`, rebuild the `traffic_hot` target, and the running city
picks up the new code. `assets/vehicles.json` and `assets/road_types.json`
reload when you save them, and the editor's Data panel edits the same rows:

```bash
./build/bin/Release/rewind_editor.exe game/traffic
```

## Where the pieces come from

| Engine | Used for |
| --- | --- |
| `sim/network.h` | roads as nodes, curved edges, lanes, junction connectors, conflicts, routing |
| `sim/network_agents.h` | cars: following, gaps, giving way, lane changes |
| `core/curve.h` | `fit_path` turns the drawn stroke into a curve |
| `project/data_table.h` | vehicle and road types in JSON, hot reloaded |
| `render/ui.h` | the HUD; its clicks become commands in the input record |
| `render/particles.h` | dust when a road is built or removed |
| `debug/editor_tool.h` | the same network in the editor's road tool |

## Next

1. **Junctions that make themselves**: a stroke crossing an existing road
   splits it and forms a crossing, instead of passing over it.
2. **Traffic lights and priority**: signals on a junction driven by an entry
   rule, give-way and stop lines, phases a player can tune.
3. **Where the traffic comes from**: buildings and zones as sources and
   sinks with demand, instead of random track to random track.
4. **Roads that mean something**: speed limits per road used in routing,
   congestion as a track penalty so cars avoid jams, one-way arrows.
5. **The look**: road textures, crossings, markings at junctions, bridges
   and tunnels from the elevation the network already carries.
6. **The game around it**: money, road costs, demand growing over time,
   goals, statistics and a saved city.
