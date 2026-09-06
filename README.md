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
- **Importers today:** PNG to a cooked texture, and Aseprite to a cooked sheet
  with the atlas, frame durations, tags, slices, tilesets and tilemap layers.
  Other types are read from source. The runtime loads by source name and
  transparently gets the cooked file when it exists.
- **Settings as overrides.** The project file stores only the app settings
  that differ from what the game's `configure()` asks for, applied on top
  of them at start. A project made from a template therefore never
  overrides a game's pixel mode or tick rate by accident.
- `rewind info` lists assets with ids and cook state; `rewind run` and
  `rewind edit` open a project in the runner or the editor, which also accept
  a project folder directly. The editor's Assets panel shows the same
  database and cooks from a button.

Next in this area: scene and prefab files, asset references as properties
with a picker, more importers, and packing cooked files into archives for
shipping.

## Editor

`rewind_editor` is the editor shell: it hosts a game DLL exactly like
`rewind_run`, but the game renders into a Scene panel and every engine tool
is a dockable window on the ImGui docking branch.

```bash
build/bin/Release/rewind_editor.exe build/bin/Release/towerdefense_hot.dll
```

- **Scene** shows the running game, integer-scaled in pixel mode, with the
  mouse mapped into game pixels. Click it to give the game your input; while
  another panel has focus the game receives nothing.
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

Multiplayer that feels local, built on the timeline. Run the `duel` sample
twice:

```bash
build/bin/Release/duel.exe --host 7777
```

```bash
build/bin/Release/duel.exe --connect 127.0.0.1:7777
```

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
  project/cooked.h    cooked texture and sheet formats (sprites, tilesets, tilemaps)
  debug/overlay.h     ImGui overlay and TimeControl
  debug/inspector.h   InspectorVisitor: reflected state as ImGui widgets
tools/runner/         rewind_run, the hot-reload host
tools/editor/         rewind_editor, the dockable editor shell
tools/cli/            rewind: new, info, cook, run, edit
samples/bounce/       shapes, entities, rewind, desync detector
samples/pixels/       sprites, animation, tilemap, sound, pixel mode, gamepad
samples/duel/         two players over rollback netcode
samples/towerdefense/ Rewind Defense: waves, towers, economy, mouse input
tests/                headless tests: core, timeline, assets, world, reflect, ecs, net, shader, text, project, hot reload
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
- **More than two players** and a lobby handshake for the session.
- **Prefabs and scene composition** on top of the world and the text format.
- Linux verified on a real machine, then macOS.
- **Editor growth**: entity list and scene editing, asset browser, undo on
  top of timeline edits, multi-viewport windows, a custom theme and font.

## License

MIT. See `LICENSE`.
