# Rewind

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
| Timeline of inputs + snapshots | Rewind, scrub, step, replay | Hot reload without losing state |
| Per-tick state fingerprint | Instant "DESYNC at tick N" when anything is non-deterministic | Cross-machine sync verification |

## Building

Requirements: CMake 3.24+, a C++20 compiler, git. Everything else (SDL3,
Dear ImGui, stb_image) is fetched automatically on first configure. The
Vulkan SDK is optional: with it, shaders are also built for Vulkan.

Windows with Visual Studio 2026:

```bash
cmake --preset vs2026
cmake --build --preset vs2026-release
```

Use `vs2022` for Visual Studio 2022, or `ninja` from a developer prompt.
The sample ends up in `build/bin/Release/bounce.exe`.

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
- `draw_text` uses a built-in 5x7 pixel font. No font file needed.
- Render targets are ordinary textures: `create_render_target()` then
  `set_target()`. Pixel mode is built on this.

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
  render/renderer2d.h batched GPU drawing: shapes, text, textures, sprites, targets
  render/sprite.h     SpriteSheet, tags, slices, Aseprite loading
  render/texture.h    Texture handle
engine/shaders/       sprite.vert / sprite.frag (SPIR-V) and sprite.hlsl (DXBC)
  world/tilemap.h     TileMap, tilemap_move collision, drawing, loading
  audio/audio.h       Sound and the polyphonic Audio player
  assets/aseprite.h   the .aseprite parser (sprites, tilesets, tilemaps)
  debug/overlay.h     ImGui overlay and TimeControl
  debug/inspector.h   InspectorVisitor: reflected state as ImGui widgets
tools/runner/         rewind_run, the hot-reload host
samples/bounce/       shapes, entities, rewind, desync detector
samples/pixels/       sprites, animation, tilemap, sound, pixel mode, gamepad
tests/                headless tests: timeline, assets, world, reflect, ecs, hot reload
```

The engine builds as a shared library so that SDL, ImGui, and the engine
exist exactly once in the process. Game DLLs link against it and carry no
copy of their own.

## Roadmap

In rough order:

- **Custom shaders and materials** on the new GPU backend: palette swaps,
  lighting, post-processing, with shader hot reload.
- **Sub-pixel camera smoothing** for pixel mode.
- **Positional audio** (pan and distance) on top of the event channel.
- **Fixed-point math option** for bit-exact determinism across machines.
- **Rollback netcode** built on `Timeline::seek` plus remote input frames.
- **Prefabs and scene composition** on top of the world and the text format.
- **Always-on frame profiler** in the overlay.
- Linux and macOS builds.
- An editor on top of all of the above.

## License

MIT. See `LICENSE`.
