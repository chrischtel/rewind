# Rewind

**A 2D game engine where you never lose your place.**

Rewind is a code-first C++20 engine built around one idea: the simulation is
deterministic and the engine remembers everything that happened. Hold a key
and the last minutes of your game run backwards. Step forward one tick at a
time. Replay a session and let the engine prove, tick by tick, that it
reproduces exactly. Rebuild your game code and the running game picks it up
without restarting. Save a sprite in Aseprite and it appears in the running
game. Play against a friend over the internet with rollback netcode that
runs on the very same machinery as the rewind key.

Rendering runs on Vulkan or Direct3D 12 through SDL's GPU API with the
engine's own shaders. Tools live in a dockable editor. Everything ships as
one CMake project that fetches its own dependencies.

> Status: early and moving fast. Windows is the developed platform; Linux
> builds in CI but has not been run on a real machine yet. The API changes
> when it should. There is no release yet.

## Who it is for

- **Solo developers and small teams making 2D games** who want a fast,
  understandable engine they can read in an afternoon and bend to their
  game, instead of a general-purpose editor with a thousand knobs.
- **People who like to program their games.** A game is a class with a
  handful of functions. No scene graph to fight, no editor required to
  start. The editor and the project tools help; they never get in the way.
- **Anyone who wants multiplayer without the pain.** Deterministic
  simulation plus rollback is built in from the start rather than bolted on.
- **Learners.** The whole engine is small, commented, and tested. Every
  system is one folder.

It is not for 3D games, and not for people who want a no-code workflow.

## What works today

**Time**
- Fixed-step deterministic simulation with a recorded timeline: rewind,
  scrub, single-step, replay with per-tick fingerprint verification, and a
  desync detector that names the exact tick where determinism broke.
- Live state edits recorded as history, so an inspector change becomes part
  of the timeline instead of a lie.

**Hot everything**
- Game code as a DLL reloaded on rebuild with the running state and the
  whole timeline intact. Shaders recompiled on save. Textures, sounds and
  Aseprite files reloaded on save, in place, pointers still valid.

**Rendering**
- SDL GPU backend on Vulkan and Direct3D 12 with the engine's own batched
  sprite pipeline; a typical frame is a handful of draw calls.
- Sprites, sprite sheets, animation, tilemaps with flips, shapes, render
  targets, pixel mode with integer scaling, TrueType text with kerning.
- Meshes: your own triangle lists with texture and vertex colour, thick
  polylines with joins, caps, dashes and UVs along the length, polygons,
  draw layers, 4x4 transforms and depth-tested passes ready for 3D.
- Curves: Bezier paths, arcs and Catmull-Rom splines with arc-length
  sampling, closest point, split, join, offset and flattening; polygon
  offset, simplification, hull and intersection helpers.
- Camera controller with smooth pan and zoom around the cursor, bounds,
  follow with dead zone, shake, culling and a level-of-detail hint; a
  spatial hash grid for rectangle, circle, segment and nearest queries.
- Gizmos: immediate-mode move, rect, rotate, point and Bezier path handles
  with grid and angle snapping, driven from reflected fields, so the editor
  places and reshapes things in the scene without game-specific code.
- Gameplay blocks: weighted graphs with deterministic A* and Dijkstra
  floods, grid graphs, steering (seek, arrive, pursue, keep distance,
  separation, path following), and reflected timers, cooldowns and state
  machines that rewind with the rest of the state.
- Materials: your own GLSL fragment shaders with extra textures and
  parameters, one source for both backends.
- Native Aseprite import: layers, tags, slices, tilesets, tilemap layers.

**Simulation and data**
- Reflection: describe your state once, get save and load, the state
  fingerprint, an inspector, JSON files and hot reload for free.
- Entity world with dense component pools and generation-checked handles.
- Deterministic random numbers, swept tilemap collision, events for sound
  and effects that never fire twice during rewinds or rollbacks.

**Multiplayer**
- Rollback netcode over UDP with input delay, prediction, redundant
  packets, stall handling and cross-peer state verification, for up to
  four players, with a lobby (host, join, ready, countdown, shared seed).

**Tooling**
- Editor with dockable Scene, Entities, Timeline, Inspector, Assets,
  Console, Profiler and Project panels. Edit and play modes (Stop returns
  to the edited state), undo and redo, a free camera, gizmos with snapping,
  right-click placement, entity names and search.
- Projects with a fixed layout, stable asset ids in sidecars, an
  incremental, content-hash-based asset cook, scenes and prefabs with
  per-instance overrides, asset references by id with a picker and
  drag-and-drop from the asset browser. A `rewind` command line for
  creating, inspecting, cooking, running and editing projects.
- Inspector widgets from hints: color pickers, points, rectangles, entity
  links, enums by name, curve previews, asset pickers.
- Always-on profiler, engine log with an in-game console, gamepad, mouse
  (with wheel) and keyboard input as part of the deterministic input record.
- Twenty-six headless test suites and a CI workflow for Windows and Ubuntu.

Four samples show all of it: `bounce`, `pixels`, `duel` (a four-player netcode arena with a lobby) and
`towerdefense`, a complete small game.

## Planned

In rough order.

- **Scene management** in the editor: new scene, scene list, unsaved
  marker, Ctrl+S, entity hierarchy.
- **Editor polish**: theme, toolbar icons, status bar, welcome screen.
- **3D and isometric tools**: 3D camera, vertex shaders in materials,
  transform hierarchies, glTF import, picking (the renderer is 3D-ready).
- **Post-processing and 2D lighting** on top of materials.
- **More importers** and packing cooked assets into archives for shipping.
- **NAT traversal** or a relay so sessions work over the internet.
- **Fixed-point math option** for bit-exact determinism between different
  CPUs over the internet.
- **Linux on a real machine**, then macOS.
- **Multi-viewport windows**, an asset browser with thumbnails, positional
  audio.

## Quick start

Requirements: CMake 3.24 or newer, a C++20 compiler, git. Everything else
is fetched on first configure. The Vulkan SDK is optional but recommended
for its shader compiler.

```bash
cmake --preset vs2026
```

```bash
cmake --build --preset vs2026-release
```

Then run `build/bin/Release/towerdefense.exe`, hold **F4**, and watch the
last wave undo itself. Presets exist for Visual Studio 2022 and Ninja too.

A game in its entirety:

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

That single file builds both a normal executable and a hot-reloadable DLL.
Three rules make everything work: `tick` depends only on its context and
the state, `reflect` lists the whole state, and side effects go through
`emit`. The engine tells you when you break them.

## Learn more

- [docs/GUIDE.md](docs/GUIDE.md): the full reference. Every system, the
  hotkeys, the layout of the source tree, and how to use each feature.
- `samples/`: read `bounce` first, then `towerdefense`.
- `tests/`: the clearest documentation of what each system guarantees.

## License

MIT. See `LICENSE`.
