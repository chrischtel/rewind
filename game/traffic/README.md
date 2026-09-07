# Traffic

A top-down traffic management sim built on the Rewind engine, in the style of
Highways & Co but European. This is a game, not part of the engine:
everything under `engine/` stays generic, and this folder is one of its users.

Towns sit on the map and every pair of them wants to be connected. Drag with
the left mouse button to draw a road: the stroke is fitted to a curve, and
anything it crosses is split so a junction appears there. Cars are the trips
between towns, not random traffic; they plan a route, obey the speed limit of
the road they are on, follow the car in front, give way and queue.

The whole city is simulation state on the fixed step, the camera included, so
F4 rewinds the traffic, F5 replays it and verifies every tick, and the
inspector can edit any car mid-jam.

## Run it

```bash
cmake --build --preset vs2026-release
./build/bin/Release/traffic.exe
```

| Input | Does |
| --- | --- |
| left drag | draw a road with the selected type |
| Roundabout tool | click to lay a roundabout; the roads it crosses become its entries |
| Signals tool | click a junction to put traffic lights on it, or take them away |
| Remove tool | click a road or junction to bulldoze it |
| right drag | pan, and cancel the road being drawn |
| wheel | zoom around the cursor |
| WASD / arrows | pan |
| F4 / F2 / F5 / F1 | rewind, pause, replay and verify, overlay |

Arguments beyond the engine's own (`--frames`, `--screenshot`, `--speed`):
`--zoom 6` and `--at X Y` set the view, `--demand 12` sets how many trips a
town pair sends per minute, `--demo 1` lays a network to look at, and
`--overlay-flow 1` starts with the congestion overlay on.

## What is in it

**The road network.** Roads are curves with lanes. A road drawn across
another splits both and forms a junction sized by the roads that meet there;
a road that ends on another makes a T; ends snap to junctions nearby.
Roundabouts are laid as a ring of one-way arcs that clears its own island.
Bulldozing puts the cars that were on the road back on the nearest lane and
re-plans their routes.

**Markings, to European standard and in real metres.** Kerb bands, edge
lines, a centre line that is dashed where overtaking is allowed and doubled
where it is not, dashed lane dividers, and at every junction approach: a
zebra crossing, give-way teeth for the minor road, a stop line where there
are signals, turn arrows read out of the junction's own connectors, and a
hatched keep-clear between opposing turn lanes.

**Traffic lights.** Opposite approaches share a phase; each phase runs green,
amber and then an all-red clearance. Cars stop at the line, cross on amber if
stopping would be harsh, and never enter a junction they cannot leave.

**Traffic that means something.** Every car is a trip between two towns. A
route that has no road sends nothing, which is what the player is there to
fix. Cars drive at the lower of what they can do and what the road allows,
and the router pays for slow roads by making them look longer than they are.

**The shell.** Money against a target, a price per metre for every road type,
the cost of the road being drawn shown next to the cursor, a build panel
whose cards are drawn by the same code that draws the roads, the routes still
to connect, a green-to-red congestion overlay, and a minimap.

**The look.** Generated grass with worn patches and scattered trees, all from
a shader and a hash, so the game needs no art at all.

## Where the pieces come from

| Engine | Used for |
| --- | --- |
| `sim/network.h` | roads as nodes, curved edges, lanes, junction connectors, conflicts, routing |
| `sim/network_agents.h` | cars: following, gaps, giving way, lane changes, entry rules |
| `core/curve.h` | `fit_path` turns the drawn stroke into a curve; offsets and sub-paths place every marking |
| `render/mesh.h` | the road geometry, built once and cached against the network's version |
| `project/data_table.h` | vehicles, road types and towns in JSON, hot reloaded |
| `render/ui.h` | the shell; its clicks become commands in the input record |
| `render/particles.h` | dust when a road is built or removed |
| `debug/editor_tool.h` | the same network in the editor's road tool |

`roads.h` is the game's own road builder: snapping, crossing detection and
the splits that turn a drawn line into a network. It has no renderer and no
game state, so `tests/roads_test.cpp` exercises it without a window.

## Next

1. **Elevation**: bridges, embankments, cuttings and tunnels on the -3..+3
   scale the reference uses; the network already carries a height per node.
2. **Better following**: an intelligent-driver model so queues ease rather
   than step, per-driver variation, and lane changes to overtake.
3. **Congestion in the routing**: feed the measured flow back as a penalty so
   cars avoid a jam, with enough damping that they do not oscillate.
4. **Where trips come from**: buildings and zones with a day and night cycle,
   instead of a flat rate per town pair.
5. **The record of the city**: graphs of trips served and travel times, a
   ranking of the junctions costing the most, and a saved city.
6. **More of the look**: road textures, bus lanes and cycle lanes, level
   crossings, and signs at the roadside.
