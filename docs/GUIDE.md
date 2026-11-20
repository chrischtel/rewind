# Rewind reference guide

The full reference for every system. For the overview, who the engine is
for, and the roadmap, see the [README](../README.md).

A 2D game engine where you never lose your place while debugging.

Rewind is a code-first C++20 library. You write a small `Game` class; the
engine gives you a fixed-step simulation that can run **backwards**, a
timeline you can scrub like a video, and a **determinism checker** that
proves every re-simulated tick matches the original. The same foundation is
what rollback netcode and hot code reload will be built on.

## Why this exists

Most engines treat time as something that only moves forward. Rewind records
every input and fingerprints every state, so a bug is never a one-off: hold a
key, watch the last two minutes run in reverse, stop at the frame that went
wrong, and step forward one tick at a time.

Three ideas reinforce each other:

| Idea | What it gives you now | What it enables later |
|------|-----------------------|-----------------------|
| Deterministic fixed-step simulation | Reproducible bugs from a tiny input log | Rollback netcode, replays, ghost racers |
| Timeline of inputs + snapshots | Rewind, scrub, step, replay, rollback netcode | Hot reload without losing state |
| Per-tick state fingerprint | Instant "DESYNC at tick N" when anything is non-deterministic | Cross-machine sync verification |

## Building

Requirements: CMake 3.24+, a C++20 compiler, git. Everything else (SDL3,
Dear ImGui, stb_image, SPIRV-Cross) is fetched automatically on first
configure. The Vulkan SDK is optional but recommended: its `glslc` builds
the engine shaders and your materials for both backends.

Windows with Visual Studio 2026:

```bash
cmake --preset vs2026
cmake --build --preset vs2026-release
```

Use `vs2022` for Visual Studio 2022, or `ninja` from a developer prompt.
The sample ends up in `build/bin/Release/bounce.exe`.

Linux: install the X11 or Wayland development packages SDL needs, plus
`glslc` for the shaders, then `cmake --preset ninja` and
`cmake --build --preset ninja`. The hot reload loader and the runner use
`dlopen` there. The GitHub Actions workflow in `.github/workflows` builds and
tests both platforms; Linux is not yet exercised on a developer machine, so
expect rough edges the first time.

## Try it

Run `bounce`. Move the square with the arrow keys or WASD, press Z or Space
to launch balls. Then:

| Key | Action |
|-----|--------|
| **F4** (hold) | Rewind through history in real time |
| **F2** | Pause / resume. Resuming from the past discards the old future. |
| **F3** | Step exactly one tick |
| **F5** | Replay everything from tick 0 and verify each tick's fingerprint |
| **F1** | Toggle the overlay |
| Esc | Quit |

The overlay has a timeline slider you can drag to any tick, and a green
"deterministic" line that turns red with the exact tick number if a replay
ever diverges. The sample has a gravity slider that is deliberately left out
of the saved state: change it, press F5, and watch the detector catch it.

## Reflection and the inspector

A game lists its state once in `reflect()`, one `field` call per member,
with optional hints (range, drag speed, tooltip, read-only, transient).
Structs opt in with the same one function; vectors, enums, strings and all
numeric types work out of the box, and the engine's own `Rng`, `InputFrame`,
`AnimState`, `Vec2`, `Rect`, `Color` and `TileMap` already describe
themselves.

That one description drives:

- **save and load**, so the timeline, hot reload and the fingerprint all work
  with no serialization code in the game;
- **the State panel in the overlay**: every field as an editable widget,
  nested structs as trees, arrays with counts. Change a value while the game
  runs and the timeline records it as an edit at that tick. Rewinding past it
  works, replaying through it restores your edit instead of flagging a desync;
- **"copy as text"**, a readable dump of the whole state for bug reports;
- later, the editor's inspector, scene files and network sync.

`transient` fields are shown but never saved, which is how the bounce sample
exposes its gravity knob while keeping it out of the state on purpose.

Property widgets: a struct announces how it wants to be drawn with
`v.hint(Widget::Color)` at the top of its `reflect`, which `Color`, `Vec2`,
`Rect` and `Entity` already do, so they appear as a color picker, compact
float rows and entity links with a select button wherever they are used.
Integer and enum fields with `{.names = kNames, .name_count = n}` draw as
dropdowns. Data visitors ignore both.

The editor's Entities panel lists every entity with its component names;
selecting one shows its components in the Inspector, each editable and
removable, with a dropdown to add more, and buttons to create, destroy,
spawn a prefab or turn the selection into one.

## Entities and components

`World` holds entities and dense, typed component pools. Components are
plain structs with a `reflect()` function; nothing to inherit, no macros.

```cpp
struct Transform { float x = 0, y = 0;  void reflect(Visitor& v) { v.field("x", x); v.field("y", y); } };
struct Velocity  { float vx = 0, vy = 0; void reflect(Visitor& v) { v.field("vx", vx); v.field("vy", vy); } };

World world;
world.register_component<Transform>("Transform");   // pins the saved layout
world.register_component<Velocity>("Velocity");

Entity e = world.create();
world.add(e, Transform{10, 20});
world.add(e, Velocity{1, 0});

world.each<Transform, Velocity>([&](Entity, Transform& t, Velocity& v) {
    t.x += v.vx * dt;
});
world.destroy(e);          // strips every component; the handle goes stale
```

The world is ordinary state: put it in your Game, list it in `reflect()`,
and it saves, rewinds, hot-reloads and appears in the inspector. Handles
carry a generation, so references to destroyed entities are detected rather
than dangling. Iteration order depends only on the sequence of operations,
which keeps it deterministic. The bounce sample's balls are entities.

## Text files

Any reflected object can be written to JSON and read back by field name:

```cpp
save_json_file("level.json", world);
load_json_file("settings.json", settings);
std::string text = to_json(game_state);
```

Missing fields keep their defaults, unknown fields are ignored, and fields
can be reordered, so files written by an older build still load. Comments
and trailing commas are tolerated. This is the format the editor will use
for scenes, prefabs and settings. Binary snapshots stay the format for the
timeline, where speed matters more than readability.

## Rendering

Rewind draws through SDL's GPU API, so underneath it is Vulkan or Direct3D
12 (Metal on macOS later) and the GPU code is ours: the shaders in
`engine/shaders`, one batched sprite pipeline, vertex and index buffers
uploaded once per frame, and one render pass per target.

- The same shader exists twice: GLSL compiled to SPIR-V at build time when
  `glslc` from the Vulkan SDK is installed, and HLSL compiled at runtime to
  DXBC for Direct3D 12. Without the Vulkan SDK the engine still runs on
  Windows; with it, both backends work.
- Pick the backend with `config.gpu_driver = "vulkan"` or `"direct3d12"`, or
  the `SDL_GPU_DRIVER` environment variable. `gpu_debug = true` turns on the
  validation layers.
- Everything drawn between `begin_frame()` and `end_frame()` is batched by
  texture. Shapes go through a 1x1 white texture, so a whole frame of tiles,
  sprites, shapes and text is usually a handful of draw calls. The overlay
  shows the count.
- Text: `draw_text` with no font uses the built-in 5x7 pixel font.
  `load_font("ui.ttf", 16)` bakes a TrueType or OpenType file at that pixel
  size into an atlas with kerning; `draw_text(font, x, y, "Héllo")` draws
  UTF-8 through the same batched pipeline, and `measure_text` sizes it for
  layout. Fonts are cached by path and size and re-baked when the file
  changes. Small sizes sample nearest for crisp pixel fonts, larger ones
  smooth. The overlay and editor take a UI font through `ui_font` in the
  app config or project file.
- Render targets are ordinary textures: `create_render_target()` then
  `set_target()`. Pixel mode is built on this.

### Meshes, lines and polygons

Everything the renderer draws is a triangle list of `MeshVertex` (position
with z, uv, colour), and you can draw your own:

```cpp
Mesh road;                                   // build once, or every frame
StrokeStyle style;
style.width = 36;
style.join = LineJoin::Round;                // Miter (with miter_limit), Bevel, Round
style.cap = LineCap::Round;                  // Butt, Square, Round
style.dash = 24; style.gap = 18;             // 0 = solid; dash_offset scrolls the pattern
style.uv_per_unit = 1.0f / 64;               // u along the length, v across: a texture repeats along the line
build_polyline(points, style, road);         // appends; closed = true makes a loop
build_polygon(outline, r, g, b, a, road);    // ear clipping, simple polygons
build_circle(center, 20, 12, 48, r, g, b, a, road); // disc or ring
renderer.draw_mesh(road, texture);           // world space, current material, vertex colour times texture
renderer.draw_polyline(points, style);       // the one-liners build and draw in one go
renderer.fill_polygon(outline, color);
```

`draw_mesh` takes an optional `Mat4` transform; `Mat4` has translation,
scaling, rotation, ortho, perspective and look-at, column-major like the
shaders. The vertex shader multiplies every position by a view-projection
matrix: pixel space by default, or your own through
`set_view_projection(&matrix)`, which is the 3D path. `set_depth_test(true)`
switches to depth-tested drawing (less-or-equal, with writes, freshly
cleared depth buffer per switch). Both are reset by `begin_frame()`.

Curves live in `core/curve.h`, independent of rendering:

```cpp
Path road = Path::from_catmull_rom(points, /*closed=*/true); // smooth through every point
Path p; p.move_to(a); p.line_to(b); p.arc_to(center, kPi / 2); p.cubic_to(c1, c2, d); p.close();
float len = road.length();                    // Gauss-Legendre arc length
std::vector<Vec2> flat; road.flatten(0.25f, flat); // adaptive, within 0.25 units
std::vector<Vec2> kerb = road.offset(16);     // parallel curve (right of travel in y-down space)
renderer.draw_path(road, style);              // flatten + draw_polyline; fill_path() for areas

PathSampler s(road);                          // arc-length table, build once
PathPoint at = s.at(distance);                // position, unit direction, segment, t; wraps on closed paths
PathPoint near = s.closest(mouse);            // exact closest point (Newton refined) and its distance along
road.split(seg, t, first, second); road.subdivide(seg, t); road.join(other); road.reversed();
```

`CubicBezier` has point, derivative, direction, normal, curvature, length,
split, sub-curve and bounds; `Arc` converts to Beziers of at most 90
degrees. Polyline helpers: `offset_polyline` (also grows or shrinks
polygons), `simplify_polyline` (Douglas-Peucker), `resample_polyline`,
`closest_point_on_segment`, `line_intersection`, `segments_intersect`,
`polygon_contains`, `convex_hull`. `Vec2` has the usual operators plus
`dot`, `cross`, `length`, `normalize`, `perp`, `lerp`, `rotate`,
`from_angle` and `angle_of`. Paths reflect, so they save, load, diff and
show up in the inspector.

### Areas

`core/polygon.h` is for filled regions with holes, and for turning a pile
of overlapping shapes into one clean partition. A `PolygonSet` is any number
of regions that do not overlap; each `Polygon` is a boundary plus its holes.

```cpp
PolygonSet lane_a = polyline_band(points_a, 3.5f);          // what a line of that width sweeps
PolygonSet lane_b = polyline_band(points_b, 3.5f);
PolygonSet road   = polygon_close(polygon_union(lane_a, lane_b), 1.5f); // one outline, corners rounded
PolygonSet kerb   = polygon_border(road, 0.3f);             // the ring just outside it
PolygonSet island = polygon_holes(road);                    // what the lanes enclose
PolygonSet stripes = polygon_hatch(island, 0.785f, 1.3f, 0.4f, 0.3f);

Mesh mesh;
build_polygon_set(road, 0.2f, 0.2f, 0.22f, 1, mesh);       // triangles, holes left open
```

- **Combining:** `polygon_union` (of raw rings wound either way, of two
  sets, or of many sets at once), `polygon_difference`,
  `polygon_intersection`, `polygon_xor`.
- **Growing and shrinking:** `polygon_offset` with round, mitered or
  bevelled corners; `polygon_close` (grow then shrink: fills gaps, rounds
  inward corners), `polygon_open` (shrink then grow: removes spikes and
  necks), `polygon_border` (the ring outside a set).
- **Lines:** `polyline_band` (butt, square or round ends; closed lines give
  rings), `clip_polyline` (the pieces of a line inside or outside a set, in
  order along the line: this is how a marking is trimmed).
- **Hatching:** `polygon_hatch` returns stripes as area, cut exactly to the
  boundary; `polygon_hatch_lines` returns their centre lines.
- **Tidying and asking:** `polygon_simplify`, `polygon_remove_small`,
  `polygon_holes`, `polygon_fill_holes`, `polygon_inner_circle` (the widest
  place in a region: where a label goes, and how wide a gap really is),
  `polygon_signed_distance`, and `area()`, `contains()`, `bounds()` on a set.

Results always have boundaries with positive `polygon_area()` and holes
negative. Everything runs on an integer grid of 1/1024 of a unit inside
(Clipper2), so the same shapes give the same points on every machine and the
functions are safe in `tick()`. `build_polygon_set` (ear cutting with holes)
and `build_polygon_outline` in `render/mesh.h` turn a set into a mesh.

### Anti-aliasing

`AppConfig::msaa` is the number of samples per pixel: 1 (off, the default,
right for pixel art), 2, 4 or 8. With more than one, every edge of every
mesh, polygon and line is smoothed, in the window and in render targets; the
nearest count the driver supports is used and `Renderer2D::msaa()` says
which. A game drawn from vector shapes wants `cfg.msaa = 4`. `--msaa N` on
the command line overrides it, and the overlay is always drawn sharp on top.

### Camera

`Camera` (x, y, zoom) is what the renderer uses: `to_screen`, `to_world`,
`Camera::centered(center, zoom, w, h)`. `CameraController` in
`render/camera.h` drives it smoothly:

```cpp
CameraController cam;                  // presentation state, lives in your Game
cam.set_view_size(w, h);
cam.set_bounds(world_rect);            // the view never leaves it
cam.follow(player, 40);                // dead zone of 40 world units
cam.zoom_step(input.wheel, mouse);     // zoom around the cursor, 1.25x per click
cam.pan(drag_delta);                   // screen pixels
cam.shake(8, 0.3f);                    // on impact
cam.update(dt);                        // or update_realtime()
renderer.set_camera(cam.camera());
if (!cam.visible(bounds, 32)) continue;   // culling
int lod = cam.lod();                      // 0 at 1:1, 1 at half, 2 at a quarter...
```

Zoom is smoothed in log space, pan exponentially; `snap()` skips the
smoothing, `frame(rect)` fits a rectangle. The mouse wheel arrives in
`InputFrame::wheel` (clicks per tick), so zoom intent can be simulation
state that rewinds and replays while the controller chases it in render().
The editor's Scene panel has a **Free camera** toggle built on the same
controller: wheel zooms around the cursor, right or middle drag pans, and
`Renderer2D::set_camera_override()` makes the game's own camera calls
inert while it is on.

### Graphs, steering, timers and state machines

Simulation building blocks in `sim/`, all deterministic and reflected:

```cpp
Graph g = Graph::grid(w, h, 16, /*diagonals=*/true, [&](u32 x, u32 y) { return !wall(x, y); });
u32 a = g.add_node({10, 10}); g.connect(a, b);          // or add_edge(a, b, cost, directed)
GraphPath route = g.shortest_path(from, to);             // A* on positions (weight 0 = Dijkstra)
std::vector<float> reach = g.distances_from(from, 200);  // Dijkstra flood, e.g. movement range
Path curve = Path::from_catmull_rom(g.positions(route)); // smooth it and follow it

PathSampler sampler(curve);                              // derived: rebuild after load
PathFollower follower;                                   // reflected: distance, speed, lookahead, done
velocity = follower.update(sampler, position, dt);
velocity = arrive(position, target, max_speed, slow_radius);
velocity += separation(position, neighbours, radius, max_speed);
heading = turn_toward(heading, angle_of(velocity), max_turn * dt);

Timer spawn; spawn.start(2.0f, /*loop=*/true);  if (spawn.tick(dt)) spawn_enemy();
Cooldown fire;  if (fire.ready()) { shoot(); fire.use(0.3f); }  fire.tick(dt);
StateMachine<Mood> fsm(Mood::Idle, kMoodNames, 3);
fsm.tick(dt); if (fsm.is(Mood::Idle) && fsm.time_in_state > 2) fsm.change(Mood::Chase);
```

Graph searches break ties by node id, so every machine finds the same
route. For many agents on big graphs: `set_edge_penalty(edge, x)` adds
runtime cost (congestion, closures) without touching the authored cost
and bumps `version()`; `bidirectional_path()` meets in the middle;
`flow_to(goal)` answers "which edge next" for every node in one search,
so a crowd heading to the same place needs no per-agent search;
`RouteCache` hands back routes until the version moves. A
`CurveNetwork` wraps the same ideas as `set_track_penalty`,
`route_affected(route)` (re-plan only when a track on the route changed
or the network was rebuilt) and a built-in cache. `seek`, `flee`, `pursue`, `keep_distance`, `separation` and `limit`
are the other steering helpers. The state machine is data only (current,
previous, time in state, transitions, a one-tick `entered` flag); the
logic is a switch in your tick, and the names show up in the inspector.

### Curve fitting

`fit_path(points, tolerance)` turns a drawn stroke into a `Path` of cubic
Beziers (Schneider's least-squares fit, splitting at the worst point until
every sample is within `tolerance`): a few segments for a smooth stroke,
more where the hand wobbled. Run `simplify_polyline` first for a noisy
mouse and pass `closed` for loops. The network tool's Draw mode is built
on it: drag a stroke in the scene view and it becomes a curved edge, from
and to existing nodes when the stroke starts or ends on one.

### Curve networks

`sim/network.h` connects curves into something agents can travel: roads
with lanes, rails, rivers, pipes, belts, footpaths. The network never
learns which.

```cpp
CurveNetwork net;
u32 a = net.add_node({0, 0}), b = net.add_node({300, 0}, /*elevation=*/8);
u32 e = net.add_curved_edge(a, b, {1, 0}, {1, 0});   // or add_edge(a, b, &path)
net.set_tracks(e, 2, 2, 4);                        // two tracks each way, 4 wide, keep right
net.rebuild();                                     // after any edit: snaps ends, derives everything

Route r = net.route(TrackRef{e, 2}, TrackRef{other, 0});   // over connectors and sideways changes
Path drive = net.route_path(r, &from, &to);                // one Path for a PathFollower
Location here; net.locate(mouse, here, 20);                // nearest track and distance along it
```

Nodes carry an elevation for bridges and dips. Edges are Bezier paths
whose ends follow their nodes; tracks are offset copies running with or
against the centreline. At every node each arriving track gets a
connector curve (tangent-continuous cubic) to each leaving track on
another edge; `set_connect_filter()` decides which pairs exist (only
straight ahead, matching lanes, no left turns, whatever the game wants).
Parallel tracks on one edge are linked sideways. From tracks, connectors
and links comes a routing graph (`route_graph()`, an ordinary `Graph`):
costs are lengths, changes add `change_penalty`. `conflicts()` lists
connector pairs at a node whose curves cross, for the game's yielding
rules. `split_edge`, `move_node`, `remove_node`, `nearest_node`,
`nearest_edge` and `edges_at` are the editing side; everything reflects,
derived data rebuilds on load. `draw_network()` in `render/network_draw.h`
shows surfaces, tracks with arrows, connectors, links, conflicts and nodes
for tools and prototypes.

### Lanes that begin and end

A track need not run the whole edge. `Track::begin` and `Track::end` cut it
short of node a and node b, and `taper` is the length over which that open
end widens from nothing:

```cpp
Track pocket{2.5f * lane, true, lane, kLeftTurn};
pocket.begin = edge_length - 60;   // only the last sixty before the junction
pocket.taper = 20;
net.edge(e).tracks.push_back(pocket);

net.edge(e).tracks[1].end = 150;   // or: this lane is dropped 150 before the far node
net.rebuild();
```

A track that stops short of a node has no connectors there; it is entered
and left sideways, and the route says so with a change step. Links join
only tracks that run side by side somewhere. `track_start(track)` is where
a track begins along its edge and `beside(from, distance, to, out)` finds
the same place on the track next to it. `split_edge` hands a partial track
to the piece it lies on, or to both, measured afresh. The surface draws the
taper as an S that keeps the side with a neighbouring lane straight and
swings the other out, and `track_edge` follows it, so a line along the
outside of a pocket follows the pocket.

### Rebuilding after an edit

`rebuild()` works out again only what an edit could have changed: the
tracks of edges that changed or meet a node that did, and the connectors
and conflicts of the nodes at their ends. It finds the changes itself, by
comparing every node and edge with what it was last time, so it does not
matter whether an edit went through the network's calls, was written
straight into `node(i)` or `edge(i)`, or came from loading a save; a rewind
that restores the same network costs next to nothing. On a grid of 3120
streets a full rebuild takes about 19 ms here and an edit about 2.6 ms.
`last_rebuild()` reports the counts.

The connect filter and the track trim are trusted to look only at the node
they are asked about and the edges meeting there. Setting either one makes
the next rebuild complete, and `invalidate()` does the same by hand.
`edges_at(node)` is a table, remade when an edge may have been rewired.

### Authored junctions

By default a node connects every arriving track to every leaving one the
connect filter lets through. A node can instead carry its own list of turns:

```cpp
net.author_node(n);                          // what is derived there now becomes its turns
net.allow_turn(n, from, to, false);          // forbid one movement (authors the node if needed)
net.allow_turn(n, from, u_turn_target);      // or add one no filter would have made
if (Turn* t = net.find_turn(n, from, to)) {  // group, tag and priority are the game's
    t->group = 3;
    t->priority = 1;
}
net.rebuild();                               // as after any edit
net.release_node(n);                         // hand it back: derived again
```

Turns are reflected state on the node, so a junction the player has edited
saves, rewinds and survives a hot reload; a callback could not. Each
connector carries its turn's `group`, `tag` and `priority`. Turns follow a
road through `split_edge`, and turns whose road or lane is gone are dropped
at the next `rebuild()`.

Lookups are tables made by `rebuild()`: `connectors_at(node)`,
`connectors_from(track)`, `connectors_into(track)`, `find_connector(from,
to)` and `conflicts_of(connector)`. A `Conflict` says how far along each of
its two connectors the meeting point lies (`at_a`, `at_b`), enough to hold a
vehicle short of the point rather than short of the junction, and
`merge_conflicts = true` also lists connectors that end on the same track
(`ConflictKind::Merge`).

### Signals

`sim/signal.h` is plans and their timing, header-only and deterministic. A
`SignalPlan` is phases in order, each a set of groups with a green time,
plus the amber, all-red and red-amber times and an offset; a `SignalState`
is where a controller is in it.

```cpp
assign_turn_groups(net, node, TurnGrouping::Approach); // one group per arriving road
net.rebuild();
SignalPlan plan = signal_plan_for(net, node, 20);      // fewest phases the conflicts allow

signal_tick(plan, state, ctx.dt, demand);              // in tick(); state is reflected
if (signal_aspect(plan, state, connector.group) != Aspect::Green) hold_the_vehicle();
```

`conflicting_groups(net, node)` lists the pairs that must never be green
together and `signal_plan_clash(plan, pairs)` finds the first phase that
breaks that, which is what an editor paints red. A phase with `max_green`
above `green` is actuated: it holds its green while `demand` is true.
`signal_state_at(plan, clock)` puts a fixed plan where it belongs at a clock
time, so junctions sharing a cycle stay in step and `offset` makes a green
wave. `signal_time_to_green` and `signal_green_left` are for countdowns and
for drivers who look ahead. A group in two phases running one after the
other keeps its green across the change-over.

### Network surfaces

`sim/network_surface.h` turns a network into the ground it occupies, with
no special cases: every track and connector sweeps a band, the bands are
united and closed up by a corner radius.

```cpp
SurfaceOptions o;
o.corner_radius = 2.5f;
NetworkSurface s = network_surface(net, o);
// s.lanes       exactly what tracks and connectors sweep
// s.paved       one outline, inward corners rounded, small gaps filled
// s.islands     gaps the lanes enclose that are big enough to stand on (not paved)
// s.keep_clear  paving no lane sweeps, wide enough to mark: hatch it
build_polygon_set(polygon_border(s.paved, 0.3f), kerb..., mesh); // rings every island too
build_polygon_set(s.paved, asphalt..., mesh);
```

A crossroads, a five-way at odd angles, a slip lane and a roundabout all
come out of the same call; the middle of a roundabout is an island without
anyone saying so. `SurfaceOptions::include` leaves tracks out (pave the
carriageway and the cycle path separately), `widen` adds a verge, and the
optional region builds only what reaches into a rectangle, for tiling a big
map. `node_surface(net, node, reach)` is one junction on its own, for an
editor preview. `track_edge` and `connector_edge` give a lane's left edge,
right edge or centre as a line, which is where its markings go.
`game/roads` (run `roads --demo 1`) draws four awkward junctions this way, a
rural T with a left-turn lane, an island and a slip lane among them, and
paints them from the same data: edge lines from the outline, centre lines through the
junction, give-way lines only on the road that has to wait, guide lines for
turns. A network measured in metres wants `net.track_tolerance` at a few
centimetres so that tight bends are drawn round.

### Network agents

`sim/network_agents.h` moves things along a network's routes:

```cpp
NetworkAgents agents;                     // reflected: put it in your Game state
u32 car = agents.spawn(net.route(from, to), 0, params, tag);
agents.set_entry_rule([&](const NetworkAgents&, u32 agent, u32 connector) {
    return signals.is_green(net.connectors()[connector].node, connector);   // your rule
});
agents.tick(net, ctx.dt);                 // in tick(): deterministic, index order
PathPoint pose = agents.pose(net, car);   // position and heading for drawing
```

Each agent follows its route piece by piece (track, connector, sideways
change), keeping `headway` seconds behind a leader and `min_gap` when
standing. Before a connector that crosses another, the entry rule decides;
the default refuses while a crossing connector has someone on it.
`NetAgentParams` holds length, speeds, acceleration, braking and gaps per
agent. Parked agents (`done`) still take up room until removed. `on_track`
and `on_connector` list who is where, `lead_of` says who an agent is
following and how far ahead, `reroute` swaps the plan of an agent that is
on the new route's first track, `stop_distance` parks it short of the end.

How an agent picks its speed is `NetAgentParams::model`:

- `FollowModel::Simple` (the default) accelerates and brakes at fixed rates
  and is never faster than it can stop within the gap in front. Exact and
  cheap: trains, belts, anything on rails.
- `FollowModel::Idm` is the Intelligent Driver Model. Acceleration fades
  smoothly towards `max_speed`, braking grows smoothly with how fast the
  gap is closing, so a queue eases to a stand, pulls away one car after
  another, and a busy road makes its own stop-and-go waves. It reads the
  same parameters (`accel` is the most it accelerates, `decel` what it
  finds comfortable, `min_gap` and `headway` what it keeps) plus
  `exponent` and `max_brake`. Give drivers different parameters and the
  traffic has character.

`NetAgent::accel` is what the agent did this tick (negative is braking: the
brake lights). The motion rule works with both models.

A change of track happens along the track, not at its end. As soon as the
track beside exists there and has a gap (bigger the faster somebody is
closing on it), the agent moves over across `change_length` while it keeps
driving, and takes up room in both tracks until it has arrived. One that
finds no gap carries on to the last place from which the change still
fits, holds there with `waiting` set, and whoever comes up behind in the
other track stops short and lets it in: lanes that merge take turns.

### Editor tools

`debug/editor_tool.h` is how a game adds modes to the editor's Scene
panel. Return your tools from `Game::editor_tools()`; the panel's picker
lists them next to Select, and the active tool's `update(ToolContext&)`
runs every frame inside the scene callback with the mouse in world and
render coordinates, button edges, modifiers, the zoom, the renderer to
draw with and the gizmos for snapping. Set `ctx.edited` when you changed
state (undo step, timeline re-snapshot) and `ctx.status` for the status
bar; `panel()` draws the tool's options. While a tool is active the game
gets no mouse and right-click belongs to the tool.

`NetworkTool` comes ready: Place (click for a node chained to the last
one, click a node to connect or start a chain, drag to move, right-click
to end), Split (click an edge) and Delete (click a node or edge), with
track layout and node radius as options and the debug overlay drawn
underneath. The bounce sample keeps a `CurveNetwork` in its state and
hands the editor one of these.

### Gizmos

`debug/gizmo.h` has immediate-mode handles for tools and debug views:

```cpp
Gizmos g;                                  // keep one across frames
GizmoInput in{mouse_render_px, left_down, alt, shift, view_hovered};
g.begin(in, renderer.camera(), view_w, view_h);
g.snap().enabled = true; g.snap().grid = 16; g.snap().angle_deg = 15;
if (g.point(1, spawn)) ...                 // round grab point, true when it moved
g.move(2, position);                       // centre = free, arrow tips = one axis
g.rect(3, trigger_area);                   // corners, edges, centre
g.rotate(4, position, heading);            // ring, snaps to the angle step
g.path(5, route);                          // anchors drag with their tangents, tangents stay aligned
g.end();
g.draw(renderer);                          // world space, sizes stay in pixels
```

Hover is resolved at `end()` and applies the next frame, like Dear ImGui.
Holding Alt bypasses snapping, Shift locks a drag to one axis;
`clicked_empty()` reports a press that hit no handle, for selection.
Drawing is a list of shapes (`shapes()`), so the logic is testable without
a GPU. `GizmoVisitor` walks reflected state and turns every struct hinted
`Vec2`, `Rect` or `Path` into a handle description; `apply()` runs the
matching gizmo and writes back. That is what the editor does for the
selected entity: hint your position struct with `v.hint(Widget::Vec2)`
and it gets a move gizmo and click-to-select in the Scene panel, with a
Snap toolbar (grid and angle). A `Path` field draws in the Inspector as a
preview with a closed flag and its control points, and in the scene as an
editable curve.

### Spatial index

`SpatialGrid<T>` in `core/spatial.h` answers "what is near here" for any
payload (entity handles, indices) with axis-aligned bounds:

```cpp
SpatialGrid<Entity> grid(64);                     // cell size about one object
auto h = grid.insert(entity, bounds);             // returns a handle
grid.update(h, new_bounds);                       // cheap while it stays in the same cells
grid.remove(h);
grid.query(rect, [](u32 h, const Entity& e, const Rect& b) { ... });
grid.query_circle(center, radius, ...);
grid.query_segment(a, b, ...);                    // line of sight, bullets
auto nearest = grid.nearest(point, max_distance); // optional handle
```

It is derived data: rebuild it from the world after a load or rewind
instead of saving it. Results come in no particular order.

Draw layers: `set_layer(n)` tags everything drawn afterwards. Within a
render target lower layers are drawn first and the recording order is kept
inside a layer, so a game can draw ground, objects and UI in whatever
order its code finds convenient. Layers are sorted per frame; batches do
not merge across layers. The bounce sample draws its road at layer -1
after the balls and still ends up underneath them.

Sort keys order draws within a layer: `set_sort_key(k)` tags everything
drawn afterwards, lower keys draw first, equal keys keep their recording
order. A top-down game sets it to each sprite's feet (`y + height`) so
what is further down the screen covers what stands behind it, regardless
of the order entities are iterated. When keys reorder anything, the index
buffer is rebuilt in the new order and neighbours with the same texture
and material merge again, so y-sorted sprites still batch.

### Game UI and commands

`render/ui.h` is the UI players see: an immediate-mode `Ui` with panels,
labels, buttons, toggles, sliders, progress bars and tooltips, drawn in
screen space through the renderer with a `Font` or the built-in pixel font,
and skinned by a `UiStyle` (colours, padding, rounding, optional nine-slice
textures for panels and buttons via `Renderer2D::draw_nine_slice`). It is
not Dear ImGui and works in pixel mode. Call it from `render()`:

```cpp
ui_.begin(&r, app.ui_input());          // mouse in render pixels, with edges
ui_.begin_panel({w - 232, 12, 220, 150}, "Bounce");
ui_.label("12 balls", true);
if (ui_.button("Launch")) app.push_command(kCmdLaunch);
ui_.tooltip("same as Z");
ui_.same_line();
if (ui_.button("Pop")) app.push_command(kCmdPop);
ui_.slider("volume", volume_, 0, 1);
ui_.progress(hp / max_hp, 204, {}, "hull");
ui_.end_panel();
ui_.end();
if (ui_.wants_mouse()) { /* the click was for the HUD, not the world */ }
```

`App::ui_input()` disables the UI while the overlay or an editor panel owns
the mouse; `wants_mouse()` tells the game to ignore clicks that landed on a
panel. Widgets flow top to bottom inside a panel, `same_line()` puts the
next one beside the last, `button_at(rect, text)` places freely, and
`begin(nullptr, in)` runs the layout and hit testing without a renderer
(that is how `ui_test` checks it).

UI is presentation and runs in `render()`, so it must not touch the
simulation directly. `App::push_command(id, value)` puts a click into the
next tick's `InputFrame` (`command`, `command_value`, one per tick): the game
handles it in `tick()` like a key, and because it is part of the input
record it rewinds, replays and reaches every peer in a session.

Every game built with the engine also understands a few command line
arguments, for scripts, CI and documentation screenshots:

```
bounce --frames 240 --screenshot shot.png --command 1@30 --speed 4 --no-vsync --no-overlay
```

`--frames N` quits after N frames, `--screenshot FILE` writes a PNG of the
last frame, `--command ID[:VALUE]@FRAME` pushes a command as if the HUD had
been clicked, `--speed X` sets the time scale, `--overlay`, `--no-overlay`
and `--no-vsync` do what they say. The log line at exit tells the frame,
the tick and the speed. Everything else in `AppConfig::args` is the game's.

`TimeControl::time_scale` (`app.time_control()`) runs the simulation at a
quarter, half, double or quadruple speed: more or fewer fixed ticks per
second, never a different tick, so the timeline and the fingerprints stay
exactly what they would be at 1x. The overlay's timeline panel and the
editor status bar show it.

#### Lists, fields, tables and charts

The same `Ui` has the pieces an editor or an analysis panel is made of.
Widgets that keep something between frames (a scroll offset, the keyboard,
an open list, a sort column) take an `id` that must be unique in the `Ui`;
the others are identified by their text like a button.

```cpp
ui_.heading("Junction 12");               // title colour, a little room above
if (ui_.tabs("view", {"Phases", "Flow"}, tab_)) { /* tab_ changed */ }
ui_.separator();

ui_.begin_scroll("phases", 140);          // 140 px tall, wheel or bar
for (int i = 0; i < phase_count; ++i)
    if (ui_.selectable(phase_name(i), i == selected_)) selected_ = i;
ui_.end_scroll();
```

`heading` and `separator` structure a panel, `tabs` is a row of buttons
with the current one in the accent colour. `selectable` is a list row across
the panel, lit when selected or under the mouse; consecutive rows touch.
`begin_scroll`/`end_scroll` make a region whose content scrolls and remember
the offset per id. The renderer has no scissor, so a widget inside shows and
reacts only while the whole of it is in view: rows appear and disappear
whole at the edges instead of being cut.

```cpp
ui_.text_field("name", name_);                            // std::string, 160 px
if (ui_.number_field("green", green_s_, 1, 5, 120))       // step 1, 5..120
    app.push_command(kCmdSetGreen, static_cast<i32>(green_s_));
if (ui_.dropdown("mode", {"Fixed", "Actuated", "Off"}, mode_)) { /* picked */ }
if (ui_.wants_keyboard()) { /* typing is text, not game keys */ }
```

A click gives a field the keyboard; a click elsewhere, Enter or Escape
takes it away, and `wants_keyboard()` says so meanwhile. `text_field` edits
the string in place and returns true when it changed. `number_field` has
`-` and `+` beside it and takes typed text as the value on Enter or when it
loses the keyboard (Escape drops it), rounded to its `decimals` and clamped.
`App::ui_input()` carries the typed text and the editing keys in `UiInput`
(`text`, `key_backspace`, `key_enter`, ...). `dropdown` shows the current
option; its list opens below, is drawn at `end()` over everything else and
owns the mouse (`wants_mouse()`) until an option or anything else is clicked.

```cpp
ui_.begin_table("links", {{"Link", 120, true}, {"Flow", 70, true}, {"Note"}});
const int column = ui_.table_sort_column();               // -1: as it comes
sort_rows(rows_, column, ui_.table_sort_descending());    // your data, your sort
for (const Row& row : rows_) {
    ui_.table_cell(row.name);
    ui_.table_cell(row.flow_text);
    ui_.table_cell(row.note);
}
ui_.end_table();
```

A table is a header and cells handed over one by one, wrapping into rows
with every other one shaded; a `UiColumn` without a width shares what the
fixed ones leave, and text too long for its cell is cut. A click on a
sortable header picks that column, a second one flips the direction. The
table only remembers the choice: the caller sorts.

```cpp
const UiSeries series[] = {
    {flow_.data(), flow_.size(), rgb(86, 156, 214), "flow"},
    {queue_.data(), queue_.size(), rgb(214, 120, 86), "queue"},
};
ui_.line_chart(series, 2, 0, 120);        // panel width, 120 px plot, range from the data
```

`line_chart` draws each series across the full width over a shared value
axis with grid lines, value labels and a legend row for the named ones;
pass `y_min` and `y_max` for a fixed range. Under the mouse a marker snaps
to the nearest sample and a tooltip lists every series' value there.

`UiStyle` has the colours these use: `field` (fields, scroll areas, the
plot), `selection`, `row_alt` and `grid`.

While a field has the keyboard, tell the engine: `app.set_ui_keyboard(ui.wants_keyboard())`
once a frame. Keys then stop reaching the simulation as buttons, and Escape
leaves the field instead of the game.

### Particles

`render/particles.h`: a `ParticleDef` says what a particle is (lifetime,
speed and spread, spawn radius, gravity and drag, size and colour over its
life, spin, an optional texture), a `ParticleSystem` holds the live ones.
`emit(def, at, count, velocity)` is a burst, `add_emitter(def, at)` a
continuous source at `def.emit_rate` per second that `move_emitter` and
`remove_emitter` control; `update(dt)` integrates and drops the dead;
`draw(r)` draws discs, `draw(r, texture_of)` sprites.

Particles are presentation: step them in `render()` with the frame's time
and they never touch the simulation, the fingerprint or the timeline. Emit
them from `on_event()`, as the bounce sample does for its sparks, and a
replay fires them again exactly where the bounce was. The system has its
own seeded `Rng`, so the same calls give the same picture, and the def is a
reflected struct with an id: keep a `DataTable<ParticleDef>` and tune
effects in the Data panel while the game runs.

### Materials

A material is a GLSL fragment shader of your own, applied to any sprites
and shapes drawn after `set_material()`:

```cmake
rewind_add_game(mygame SOURCES mygame.cpp SHADERS shaders/palette.frag)
```

```cpp
Material* m = renderer.load_material("shaders/palette.frag.spv");   // in load_assets()
renderer.set_material(m);                                           // in render()
renderer.set_material_params(Params{0.5f});                         // set 3, binding 0
renderer.set_material_texture(1, palette);                          // set 2, binding 1
renderer.draw_sprite(...);
renderer.set_material(nullptr);
```

The shader receives the sprite texture at set 2 binding 0, up to three
extra textures at bindings 1 to 3, an optional uniform block at set 3
binding 0 (up to 256 bytes), and the interpolated `v_uv` and `v_color`.
The build compiles it to SPIR-V with glslc. Vulkan loads that directly; on
Direct3D 12 the engine cross-compiles the SPIR-V to HLSL with SPIRV-Cross
and then to DXBC at load time, so one shader source serves both backends.
Save the `.frag` while the game runs and it recompiles live; a shader that
fails to compile keeps its previous version and reports in the log and in
`material->error`. The pixels sample's X key cycles palette swaps.

## Sprites, pixel mode, Aseprite

Run `pixels` for the second sample: a 320x180 scene scaled up by whole
numbers, a hand-built sprite sheet with idle, walk and jump tags, parallax
stars, and gamepad support. The animation state is plain data inside the
game state, so it rewinds with everything else.

- **Textures**: `renderer.load_texture("hero.png")` or
  `renderer.create_texture(name, w, h, rgba)`. Cached by name, so calling
  again from `load_assets()` after a hot reload returns the same texture.
- **Sprites**: `renderer.draw_sprite(texture, src, x, y, options)` with
  scale, flip, rotation, origin and tint.
- **Pixel mode**: set `render_width` and `render_height` in `configure()`.
  The game draws into that small target and the engine scales it into the
  window, integer-scaled and letterboxed by default, nearest-neighbour
  filtered. `renderer.window_to_world(mouse_x, mouse_y)` maps back.
- **Aseprite files, natively**: `load_aseprite_sheet(renderer, "hero.aseprite", sheet)`
  reads the `.aseprite` file directly, no export step. Layers and groups are
  composited with visibility and opacity, frames are packed into one texture,
  timeline tags become animations, and slices become named rectangles you can
  use as hitboxes or attachment points. RGBA, grayscale and indexed sprites,
  zlib and linked cels are all handled.
- **Animation**: `AnimState` in your state, `anim_play(state, sheet, "walk")`
  in `tick()`, `anim_frame(state, sheet)` in `render()`. Forward, reverse,
  ping-pong and repeat counts all follow the tag settings.

## Tilemaps

`TileMap` is a grid of tile ids over a tileset texture, with flip flags per
cell. Build one from text rows for prototyping, or load a tilemap layer
straight out of an `.aseprite` file:

```cpp
map = tilemap_from_text({"....", "#..#", "####"}, "#", 16, 16);
load_aseprite_tilemap(renderer, "level.aseprite", "ground", map);
```

`tilemap_move(map, box, dx, dy)` moves an axis-aligned box through the map
and stops at solid tiles, resolving x before y the way platformers expect.
It returns which sides touched something, so landing, bumping and head hits
are one `if` each. `draw_tilemap` culls to the camera.

## Audio

Sounds never come from inside the simulation. A tick calls
`ctx.emit(kJump)`; the engine collects those events and hands them to
`Game::on_event()` only for ticks that ran live. Rewinding, scrubbing, replay
and rollback re-run `tick()` with no event queue, so a jump sound plays
exactly once no matter how often that tick is re-simulated.

```cpp
void tick(const TickContext& c) override { if (jumped) c.emit(kJump); }
void on_event(const SimEvent& e, Audio& audio) override {
    if (e.id == kJump) audio.play(jump_sound, 0.6f);
}
```

`Audio` loads WAV files, accepts float samples you generate yourself, plays
them polyphonically, and is cached by name so hot reloads keep the pointers.

Positional sound: `set_listener({position, max_distance, near_distance})`
says where the player's ears are, and `play_at(sound, position)` plays a
sound there: full volume within `near_distance`, silent beyond
`max_distance`, linear in between, panned left or right by where the
source is (constant power, baked into a stereo copy when the voice starts).
Both return a voice id: `move_voice(id, position)` follows a moving source,
`set_listener` re-evaluates every positional voice, `stop(id)` and
`playing(id)` do what they say. `positional_gain` and `positional_pan` are
the same curves as free functions, for your own mixing or tests.

## Asset hot reload

Every file the engine reads for you is watched: PNGs through
`load_texture`, WAVs through `load_wav`, and `.aseprite` files through the
sprite sheet and tilemap loaders. Save the file in your image or sound
editor and the running game picks it up within a second. Under the hood the
engine marks the changed files and calls your `load_assets()` again; cached
textures and sounds are refreshed in place, so pointers your game holds stay
valid even when an image changes size. A file that fails to load keeps its
last good version. This works in every build, not only under `rewind_run`.

## Log and console

`RW_LOG_INFO("mygame", "spawned %d enemies", n)` and its debug, warn and
error siblings write to stderr, to the debugger output on Windows, and into
a ring buffer the overlay shows as a console with text filtering, level
toggles and follow mode. SDL's own messages land there too under the "sdl"
category. The console header counts warnings and errors so a problem is
visible even when the section is collapsed. Every entry carries the frame
number it happened on, which lines up with the timeline when reading a bug
report.

## Profiler

Always on, in every build. The App times events, simulation, audio, render
and submit; the timeline adds `tick` and `snapshot`; games add their own with
one line:

```cpp
void tick(const TickContext& c) override {
    RW_PROFILE_SCOPE("my.tick");
    ...
}
```

The overlay's Profiler section shows a frame-time graph of the last 240
frames, a bar timeline of the last frame nested by depth (hover for exact
times), and totals per region. The cost is one clock read per scope, so
leaving scopes in shipping code is fine.

## Projects and the asset pipeline

A project is a folder with a fixed layout and a `project.json`:

```
project.json        name, game module, startup scene, app setting overrides
assets/             source assets, each with a .meta.json sidecar
scenes/             scene and prefab files (JSON)
shaders/            GLSL materials
build/cooked/       cooked assets, one file per source, regenerated
```

```bash
build/bin/Release/rewind.exe new mygame "My Game"
```

```bash
build/bin/Release/rewind.exe cook mygame
```

- **Stable ids.** Every asset gets a random id in its sidecar the first time
  the database sees it. Move or rename a file together with its sidecar and
  every reference keeps working. The sidecar also holds import settings,
  such as nearest or linear sampling for a texture.
- **Content-hash cooking.** The database hashes each file plus its import
  settings and cooks only what differs from the last cook, so a fresh clone
  or a `git checkout` re-cooks exactly what changed and nothing else. Cooked
  files are loose, mirroring the asset layout with `.cooked` appended, with a
  magic number so loaders recognise them by content.
- **Importers:** PNG to a cooked texture; Aseprite to a cooked sheet with
  the atlas, frame durations, tags, slices, tilesets and tilemap layers;
  WAV and Ogg Vorbis to cooked 16-bit PCM (`audio.mono` in the sidecar
  downmixes); GLSL in the project's `shaders/` folder to SPIR-V through
  glslc (a loose `.frag` also compiles when a material loads it). Fonts
  and data files are read from source. The runtime loads by source name
  (`load_texture("sprites/hero.png")`, `load_sound("sfx/jump.ogg")`,
  `load_material("shaders/glow.frag")`) and transparently gets the cooked
  file when it exists.
- **Import settings** live in the sidecar and show in the Inspector when an
  asset is selected in the Assets panel (with a preview for textures);
  changing one saves the sidecar and marks the asset stale, since the
  content hash covers the settings.
- **Settings as overrides.** The project file stores only the app settings
  that differ from what the game's `configure()` asks for, applied on top
  of them at start. A project made from a template therefore never
  overrides a game's pixel mode or tick rate by accident.
- `rewind info` lists assets with ids and cook state; `rewind run` and
  `rewind edit` open a project in the runner or the editor, which also accept
  a project folder directly. The editor's Assets panel shows the same
  database and cooks from a button.
- **Shipping.** `rewind pack mygame` cooks what changed and writes
  `build/mygame.rwpak`: every cooked file, every uncooked source asset,
  the scenes and prefabs, and `project.json`. Ship the runner, the engine
  DLL, the game DLL and the pack in one folder. The runner mounts every
  `.rwpak` next to the executable or the DLL, takes the app settings and
  startup scene from the packed `project.json`, and every loader reads
  from the pack first (`read_asset_bytes` in `core/paths.h` is the one
  call they share), so nothing else needs to be on disk. Packs are plain
  sequential files with a name table (`project/pack.h`); mount more of
  them with `add_asset_pack`, later mounts win.

### Asset references

State, scenes and prefabs point at assets with an `AssetRef`: the asset's
stable id from its sidecar plus the path as a readable fallback. The id
survives renames and moves (the sidecar moves with the file), the path
keeps files diffable and works without a project.

```cpp
struct Enemy {
    AssetRef sprite;                          // {"id": "3f9a...", "path": "sprites/enemy.png"}
    AssetRef hit_sound;
    void reflect(Visitor& v) {
        v.field("sprite", sprite, {.asset_type = "texture"});   // the picker offers only textures
        v.field("hit_sound", hit_sound, {.asset_type = "audio"});
    }
};

Texture* tex = renderer.load_texture(enemy.sprite);   // by id, then path, then the asset roots
Sound* snd = audio.load_wav(enemy.hit_sound);         // nullptr with a warning when unresolved
```

`use_project_paths()` fills the global `AssetIndex` from the sidecars in
`assets/` (only the small `.meta.json` files are read), and the editor
keeps it in step with its database. `resolve_asset(ref)` gives the file
to load, cooked variants first. In the Inspector an `AssetRef` is a
button showing the path (red when the id is unknown to the project, with
an arrow when the file moved), a picker with a filter, a clear button,
and a drop target: drag a row out of the Assets panel onto it. Rows there
also offer "Copy id" and "Copy path". `AssetIndex::refresh(ref)` updates
a reference after a move, for tools that rewrite files.

### Data tables

`project/data_table.h` holds a game's definitions, the unit types, items,
tiles, recipes or waves that designers tune, as rows in a JSON file, one
row per id. A row is any reflected struct with a `std::string id`, so it
can carry numbers, names, colours, nested structs, arrays and `AssetRef`s:

```cpp
struct UnitDef {
    std::string id;
    float speed = 1;
    Stats stats;
    AssetRef sprite;
    void reflect(Visitor& v) {
        v.field("id", id);
        v.field("speed", speed);
        v.field("stats", stats);
        v.field("sprite", sprite, {.asset_type = "texture"});
    }
};

DataTable<UnitDef> units{"units", "data/units.json"};
units.load();                              // through the asset roots and packs
const UnitDef* tank = units.find("tank");  // or at(i), index_of(id), rows()
```

The file is `{"rows": [{"id": "tank", ...}, ...]}` or a bare array; missing
fields keep their defaults, unknown fields are ignored. Empty and duplicate
ids end up in `problems()` instead of failing the load. `add_row`,
`duplicate_row`, `remove_row` and `save()` edit and write it back, and
`changed()` after editing a row's id through `at()` rebuilds the index.

A `DefRef<UnitDef>` is a reference to a row by id. It saves, hashes and
compares as the string, and `get(table)` resolves it, cached until the
table's `version()` changes, so a reloaded table never leaves a stale
pointer behind. Tables register by name while they exist
(`DataTableBase::find`, `DataTable<T>::find_table`), which lets a field
name its table for the inspector: `v.field("unit", unit, {.table =
"units"})` draws a filterable dropdown of ids, red when the id is not in
the table.

Return the tables from `Game::data_tables()` and the engine looks after
them: the file is watched like any other asset and reloaded when it
changes on disk (edit it with the game running, the next spawn uses the
new numbers), and the editor's **Data** panel lists them with their rows,
adds, duplicates and removes rows, edits the selected one through the
inspector and saves the file.

Definitions are data, not simulation state: they are not on the timeline
and not part of the fingerprint. Changing them mid-replay changes the
replay, exactly like changing code. The bounce sample keeps its ball
kinds in `samples/bounce/assets/ball_kinds.json`; `rewind_add_game(...
ASSETS assets)` makes that folder an asset root while developing (for
shipping, cook and pack it).

### Scenes and prefabs

A scene file in `scenes/` lists entities with their components by name, as
JSON, plus a free-form settings block:

```cpp
Scene scene = capture_scene(world, "level1", &prefabs);   // snapshot
save_scene("scenes/level1.json", scene);
load_scene("scenes/level1.json", scene);
apply_scene(scene, world, &prefabs, &warnings);            // replaces the world
```

Entity handles are restored exactly, so references between entities stay
valid after a round trip. Unknown component names and missing prefabs are
reported as warnings and skipped rather than failing the load. A settings
block carries whatever the game declares: `set_scene_settings(scene, my)`
and `scene_settings(scene, my)` go through reflection.

Prefabs are entity templates in `scenes/prefabs/*.json`, made with
`make_prefab(world, entity, id, name)` and spawned with `spawn_prefab`. An
instance carries a `PrefabLink` component (register it in your world), and
a scene stores only the fields an instance changed plus any components it
removed; editing the prefab therefore changes every instance that did not
override that field.

The editor's File menu saves and loads scenes for games that expose their
world through `Game::world()`, and `startup_scene` in `project.json` loads
one at start in both the editor and the runner. The world also gained
per-entity, by-name component access (`component_names`, `add_component`,
`reflect_component`, `visit_entity`) for tools.

Still deferred in this area: asset references as properties with a picker,
more importers, and packing cooked files into archives for shipping.

## Editor

`rewind_editor` is the editor shell: it hosts a game DLL exactly like
`rewind_run`, but the game renders into a Scene panel and every engine tool
is a dockable window on the ImGui docking branch.

```bash
build/bin/Release/rewind_editor.exe build/bin/Release/towerdefense_hot.dll
```

Started without arguments it shows a welcome screen: open a project
folder or a game DLL through the system's dialogs, create a new project
(folder plus its game DLL), or pick from the recent list, which lives in
`editor_recent.json` next to the executable. A status bar along the
bottom shows the mode, tick, frame rate, project and scene, the selected
entity and the last message; the theme is `apply_editor_theme()` from
`debug/overlay.h`, which any tool built on the engine can reuse.
`core/system.h` wraps the folder and file dialogs, process launching and
URL opening for such tools.

- **Scene** shows the game, integer-scaled in pixel mode, with the mouse
  mapped into game pixels. The editor opens in edit mode: **Play** runs the
  simulation from the edited state, **Pause** and **Step** freeze it, and
  **Stop** brings the edited state back, so you can try a change and return
  to it. **Undo** and **Redo** (Ctrl+Z, Ctrl+Y) cover every edit, whether
  it came from the Inspector, a gizmo drag or the placement menu; a drag is
  one undo step. Click the view to give the game your input; while another
  panel has focus the game receives nothing. The second toolbar row has the
  **Free camera** (wheel zooms around the cursor, right or middle drag
  pans) and **Gizmos** with a snap grid and angle: the selected entity's
  hinted `Vec2`, `Rect` and `Path` fields get handles in the view, and a
  click near an entity's position selects it. A right-click opens the
  placement menu: create an entity or spawn a prefab at that spot, move or
  duplicate the selection there, delete it (also Del and Ctrl+D).
- **Entities** lists every entity with its name, which is the first string
  field among its components, and a filter box that matches names,
  component names and `#index`. When the game registers the engine's
  `Parent` component (`sim/hierarchy.h`), the list becomes a tree: drag a
  row onto another to make it a child, drop it on the line at the bottom
  to make it a root again, and deleting an entity takes its children with
  it. The link is organisational; nothing moves children unless your tick
  does, and `parent_of`, `children_of`, `set_parent` (refuses cycles) and
  `descendants_of` are there for that.
- **Scenes** lists the files in `scenes/`. Type a name and press New for an
  empty scene, double-click to open one, right-click to make it the
  startup scene. The current scene shows a `*` while it has unsaved
  changes, Ctrl+S saves it, and opening another scene first asks whether
  to save or discard. Play and Stop leave the scene alone: Stop returns to
  the state Play started from, which is the edited scene.
- **Timeline** holds the transport, scrubber, determinism status, hot reload
  and network state. **Inspector** holds the game's debug UI and its whole
  reflected state, editable live. **Console** and **Profiler** are the same
  tools as in the overlay. **Keys** lists the hotkeys.
- **Project** shows the settings the game asked for in `configure()`. Edit
  them and save to `project.json` next to the DLL; the editor applies that
  file over the game's defaults on the next start. This is the seed of the
  project settings and asset pipeline work.
- **File** saves and loads the entire simulation state as JSON, and rebuilding
  the game target swaps the code in with the state intact, as always.

The layout resets from the View menu. Under the hood the App has an embedded
mode: it renders into a texture that a host shows anywhere, and the host
draws its own UI through a callback, so other tools can be built the same way.

## Rewind Defense

A complete small game on top of everything above: `towerdefense`. Enemies
walk a path, you place arrow, cannon and frost towers with the mouse,
upgrade them by clicking again, sell with the right button, and call waves
early for bonus gold. Waves escalate and a boss arrives every tenth. Lose
the gate and the game tells you to hold F4: rewinding three minutes of
history and replaying a wave with different towers is the whole point.

Everything in it is deterministic. The mouse is part of the input frame
(`mouse_x`, `mouse_y` in render-target pixels, `Button::MouseLeft`,
`MouseRight`, `MouseMiddle`), so every click is recorded, rewinds, replays
and would roll back over the network like any other input. Enemies, towers,
projectiles and particles are entities; the whole run is a reflected world,
so the inspector can edit gold or lives mid-game and the timeline records
that as an edit.

## Rollback netcode

Multiplayer that feels local, built on the timeline, for up to four
players. Run the `duel` sample (an arena) several times:

```bash
build/bin/Release/duel.exe --host 7777 --name Ann --players 4
```

```bash
build/bin/Release/duel.exe --join 127.0.0.1:7777 --name Bob
```

The first copy hosts a lobby; the others join it by address, press Z to
be ready, and the host presses Enter to start a three-second countdown.
Every peer then restarts at tick 0 with the same roster and seed and the
session takes over.

**Lobby** (`net/lobby.h`): `host(name, max_players)` or `join(address,
name)`, `update(now_ms)` every frame, `set_ready()`, `start()` on the
host; `state()` walks Joining, Waiting, Starting, Started or Failed. When
started, `connect_peers(transport)` adds every other player as a UDP
peer and `session_config()` gives the Session its player count and slot;
`App::restart(&session)` reinitialises the game and the timeline so all
peers begin together. Slots are player ids, the host is player 0, and
players see each other's addresses as the host sees them (LAN or direct
connections; NAT traversal is not built in). The lobby speaks small
repeated messages over the same UDP socket, so lost packets need no
handling.

**Transports**: `UdpTransport` is one socket to any number of peers
(`AddressedTransport`: `send_to`, `receive_from`, `add_peer`);
`LoopbackMesh` is the in-process equivalent for tests, with the same fake
latency, jitter and loss as `LoopbackPair`. The session sends every
player's inputs to every peer and tracks acknowledgements per player.

How it works: every tick needs every player's input. The local player's
input is known a few ticks ahead thanks to a small input delay. The remote
player's input is predicted (they keep doing what they did last) until the
real one arrives over UDP. When the truth contradicts the guess, the session
corrects the recorded tick and the timeline re-simulates from there with
history rewritten, which is the same code path as rewinding. Inputs are sent
redundantly, so lost packets need no retransmission. Confirmed states are
fingerprinted and compared between peers, so a determinism bug shows up as a
desync warning in the overlay with the exact tick.

In game code, nothing is network-aware. `tick()` reads `ctx.player(0)` and
`ctx.player(1)`; the session fills them in. Attach one in `Game::start()`:

```cpp
void start(App& app) override {
    udp.open(7777);                       // or udp.open(0, "192.168.1.5", 7777)
    net::SessionConfig cfg;
    cfg.local_player = 0;
    cfg.input_delay = 2;
    session = std::make_unique<net::Session>(cfg, udp);
    app.set_session(session.get());
}
```

The overlay shows connection state, round trip time, rollbacks, how many
ticks are currently running on prediction, and stalls. The loopback
transport in `net/transport.h` simulates latency, jitter and loss with a
virtual clock, which is how the tests drive two sessions against each other
headlessly.

## Hot reload

Every game built with `rewind_add_game(name ...)` also produces `name_hot.dll`.
Run it through the runner instead of the executable:

```bash
build/bin/Release/rewind_run.exe build/bin/Release/bounce_hot.dll
```

Leave it running. Change something in `samples/bounce/bounce.cpp` (a color,
the player speed, the physics), then rebuild only the game:

```bash
cmake --build --preset vs2026-release --target bounce_hot
```

Within a second the running game is executing the new code. The player is
where you left it, the balls keep flying, and the full timeline is still
there, so you can rewind straight through the moment the code changed.

How it works: the runner never loads the DLL you build. It loads a numbered
copy, so the linker can always overwrite the original. When the file's
timestamp changes and settles, the engine asks the old build to `save()`,
loads the new copy beside it, hands the bytes to the new build's `load()`,
and only then unloads the old code. If the new build cannot read the old
layout (you added a field to the state), the engine says so in the overlay
and starts a fresh timeline instead of guessing. If the new DLL fails to
load at all, the old one keeps running and the error shows in the overlay.

Two limits to know about: only the game DLL can be rebuilt while the runner
is running, since the engine DLL and runner are locked by the process. And a
game must not keep simulation state anywhere except what `save()` writes,
because that is the only thing carried across.

## Save games, screenshots, localisation

**Save games** are the reflected state behind a small header: the
project's `save_version`, a label, the tick and a timestamp, in
`<user data>/saves/<slot>.rwsave`.

```cpp
app.save_game("slot1", "before the boss");
app.load_game("slot1");                 // becomes the current tick; history continues from it
for (const SaveInfo& s : app.saved_games()) ...   // newest first: label, version, timestamp, tick
```

Bump `save_version` in `configure()` (or the project) when the state
changes shape and override `Game::migrate_save(from, bytes)` to rewrite
older saves; without a migration an old file is refused with a clear
error instead of read as garbage. The overlay's **Saves** section and the
editor's **Save games** panel list and load slots.

**Screenshots:** F12 or `app.screenshot()` writes the next frame (the
game only, no overlay) as PNG into `<user data>/screenshots/`. Underneath
it is `Renderer2D::read_texture()`, which copies any texture back to RGBA.

**Localisation:** string tables are flat JSON files per language in
`strings/` (nested objects flatten to `menu.play`), read through the asset
roots and packs.

```cpp
Localization::global().configure("strings", "en");   // fallback language
Localization::global().set_language("de");
draw_text(font, x, y, tr("menu.play"));
draw_text(font, x, y, trf("hello", {{"name", player_name}}));   // {name} placeholders
```

Missing keys come back as `[key]` so they are visible in the game, the
fallback fills gaps in a translation, and `available()` lists the
languages that have a table.

## Settings: project, player, bindings

Three layers, each overriding the last:

1. **The game's defaults** from `configure()`.
2. **The project** (`project.json`, `app` block): window, rendering,
   timeline, and now `organization`, `fullscreen`, `input` bindings and the
   `audio_mix`.
3. **The player** (`settings.json` in the OS's per-user folder for the
   organisation and title, `user_data_dir()`): fullscreen, vsync, volumes,
   their own bindings, and a free-form `game` block for your options.

```cpp
PlayerSettings& s = app.player_settings();
s.audio.music = 0.5f;
s.input.add_key("a", "Space");     // names as SDL spells them; "dpup", "leftshoulder" for pads
app.apply_player_settings();       // window, mix and bindings take effect now
app.save_player_settings();

audio.play(snd, 1.0f, kAudioMusic); // groups: kAudioSfx, kAudioMusic, kAudioUi times master
```

`InputBindings` maps each abstract Button to key and gamepad names and
compiles to lookup tables; `InputBindings::defaults()` is the arrows/WASD
layout every sample uses. The overlay's **Settings** section and the
editor's **Player settings** panel edit all of this live: sliders, a
fullscreen toggle, and press-to-bind (click a button's `+`, press a key or
pad button, `x` removes one). Bindings are part of the deterministic
input record only through the resulting InputFrame, so a replay is not
affected by how the player rebinds.

## Writing a game

```cpp
#include <rewind/rewind.h>
using namespace rw;

class MyGame : public Game {
    float x = 0;
    Rng rng{42};

    void tick(const TickContext& c) override {
        if (c.input.down(Button::Right)) x += 200.0f * c.dt;
    }
    void reflect(Visitor& v) override {
        v.field("x", x, {.min = 0, .max = 1280});
        v.field("rng", rng);
    }
    void render(Renderer2D& r, float) override {
        r.clear(rgb(20, 20, 30));
        r.fill_rect(x, 100, 32, 32, rgb(255, 255, 255));
    }
};

REWIND_GAME(MyGame)
```

`REWIND_GAME` becomes `main()` in the executable build and the DLL entry
points in the hot-reload build. Override `configure()` to set the window
title, size, tick rate, and history length. Register the game in CMake with:

```cmake
rewind_add_game(mygame SOURCES mygame.cpp)
```

The rules that make everything work:

1. **`tick` only depends on `TickContext` and the state itself.** No wall
   clock, no `rand()`, no reading the keyboard, no mutable globals. Use the
   provided `Rng` and store it in your state.
2. **`reflect` describes the whole state.** If a value influences the
   simulation, it must be listed. The desync detector will tell you when you
   forget one. Hand-written `save` and `load` are still allowed for special
   formats.
3. **`render` never writes state.**
4. **Side effects go through `emit()`.** Sounds, rumble and screen shake are
   requested from `tick()` and performed in `on_event()`, which only runs for
   live ticks.

## Layout

```
engine/include/rewind/
  core/app.h          window, fixed-step loop, pixel mode, gamepad, hotkeys
  core/file_watcher.h polls tracked files, drives asset hot reload
  core/log.h          RW_LOG_* macros, ring buffer behind the console
  core/profiler.h     RW_PROFILE_SCOPE and the frame profiler
  core/game_module.h  loads a game DLL and hot-swaps it on rebuild
  core/math.h         Vec2, Rect, small float helpers
  core/types.h        integer aliases, Tick
  game_entry.h        REWIND_GAME(...) macro
  sim/game.h          the Game interface
  sim/timeline.h      input log + snapshots + fingerprints, seek/advance
  sim/input.h         InputFrame, the only input the simulation sees
  sim/animation.h     AnimState and the deterministic animation player
  sim/events.h        SimEvent / EventQueue, the tick-to-world channel
  sim/reflect.h       Visitor, FieldMeta, Save/Load/Dump visitors
  sim/world.h         Entity, World, component pools
  sim/text_format.h   JSON writer/loader over reflection, file helpers
  sim/serialize.h     Writer / Reader / hash_bytes
  sim/rng.h           PCG32, trivially copyable
  render/renderer2d.h batched GPU drawing: shapes, text, textures, sprites, targets, materials
  render/font.h       FontAtlas baking, UTF-8, Font
  render/material.h   Material: a custom fragment shader
  render/shader_cross.h SPIR-V reflection and SPIR-V -> HLSL
  render/sprite.h     SpriteSheet, tags, slices, Aseprite loading
  render/texture.h    Texture handle
engine/shaders/       sprite.vert / sprite.frag (SPIR-V) and sprite.hlsl (DXBC)
  world/tilemap.h     TileMap, tilemap_move collision, drawing, loading
  net/session.h       rollback Session, packet format
  net/transport.h     Transport interface, LoopbackPair for tests
  net/udp.h           UdpTransport
  audio/audio.h       Sound and the polyphonic Audio player
  assets/aseprite.h   the .aseprite parser
  assets/png.h        tiny PNG encoder for templates, tests and screenshots
  project/project.h   Project file and layout, path setup
  project/asset_db.h  AssetDatabase: sidecars, ids, hashing, cooking
  project/cooked.h    cooked texture and sheet formats
  project/scene.h     Scene, Prefab, PrefabLibrary, capture/apply, files (sprites, tilesets, tilemaps)
  debug/overlay.h     ImGui overlay and TimeControl
  debug/inspector.h   InspectorVisitor: reflected state as ImGui widgets
tools/runner/         rewind_run, the hot-reload host
tools/editor/         rewind_editor, the dockable editor shell
tools/cli/            rewind: new, info, cook, run, edit
samples/bounce/       shapes, entities, rewind, desync detector
samples/pixels/       sprites, animation, tilemap, sound, pixel mode, gamepad
samples/duel/         up to four players over rollback netcode, with a lobby
samples/towerdefense/ Rewind Defense: waves, towers, economy, mouse input
tests/                headless tests: core, timeline, assets, world, reflect, ecs, net, shader, text, project, scene, hot reload
```

The engine builds as a shared library so that SDL, ImGui, and the engine
exist exactly once in the process. Game DLLs link against it and carry no
copy of their own.

## Roadmap

In rough order:

- **Post-processing pass** (full-screen materials over the render target)
  and 2D lighting on top of materials.
- **Sub-pixel camera smoothing** for pixel mode.
- **Positional audio** (pan and distance) on top of the event channel.
- **Fixed-point math option** for bit-exact determinism across machines,
  which internet play between different CPUs will eventually need.
- **NAT traversal** or a relay for sessions over the internet.
- **Prefabs and scene composition** on top of the world and the text format.
- Linux verified on a real machine, then macOS.
- **Editor growth**: entity list and scene editing, asset browser, undo on
  top of timeline edits, multi-viewport windows, a custom theme and font.

## License

MIT. See `LICENSE`.
