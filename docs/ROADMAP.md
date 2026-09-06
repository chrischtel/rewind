# Roadmap

One list at a time. A list is finished, or parked in a stable state, before
the next one starts.

## Active: project infrastructure

Decided together (2026-09-06): stable asset ids in sidecars, loose cooked
files, fixed project layout, textures and Aseprite cooked first. Done so far:
project file with settings overrides, asset database, content-hash cooking,
`rewind` CLI, editor Assets and Project panels.

Remaining, in order:

1. **Scenes and prefabs.** Done: `project/scene.h`, scene_test, editor
   File menu, startup scene in runner and editor.
   - A scene file is JSON in `scenes/`: scene name, the reflected `World`,
     and a free-form settings block for the game (spawn points, ambient
     values) that the game declares with `reflect`.
   - A prefab is a scene fragment with an id: one root entity plus its
     components, saved as JSON in `scenes/prefabs/`.
   - A scene instance of a prefab stores the prefab id and only the fields
     that differ, using the existing `json_diff` and `apply_json`.
   - Engine API: `Scene` struct, `save_scene`, `load_scene`, `spawn_prefab`,
     `Game::load_scene(const Scene&)` hook. Tests: round trip, prefab
     override, missing prefab handled.
   - Editor: File menu save/load scene, startup scene from `project.json`.

2. **Entity list and basic property widgets.** Done: Entities panel
   (select, create, destroy, spawn prefab, make prefab), per-entity
   components in the Inspector with add and remove, widget hints (color
   picker, Vec2, Rect, entity links with select), names for enum fields.

**This list is parked.** Everything below in "deferred" stays deferred until
the next list is done.
   - `World` gets `visit_entity(Entity, Visitor&)` to reflect one entity's
     components by pool name.
   - Editor Entities panel: list, select, per-entity components in the
     inspector, create and destroy, add and remove component by name.
   - Widgets: enums by name (`FieldMeta` gains an `enum_names` list), color
     picker for `Color`, compact `Vec2` and `Rect`, entity links that select
     on click.

After these two, this list is parked. Deferred on purpose (all clean to add
later, nothing depends on them): asset references with a picker, importers
for audio, fonts and shaders, archives for shipping, undo on top of timeline
edits.

## Done: generic engine functionality

Rendering, geometry, spatial queries, editor interaction and simulation
building blocks that any game uses. Engine-level only, never game-specific.

1. ~~Mesh drawing (triangle lists with texture, UVs, vertex color), polyline
   and ribbon builders with joins, caps and dashes, draw layers and sort keys.
   Built 3D-ready from the start: positions with z, 4x4 transforms, optional
   depth-tested passes.~~ Done: `render/mesh.h`, `Mat4`, `draw_mesh`,
   `set_layer`, `set_view_projection`, `set_depth_test`, `geometry_test`.
2. ~~Curves: Bezier and Catmull-Rom, arcs, arc length, tangents, offset,
   split and join, closest point. Polygon triangulation and offset.~~
   Done: `core/curve.h` (CubicBezier, Arc, Path, PathSampler, polyline and
   polygon helpers), `draw_path`/`fill_path`, `curve_test`.
3. ~~Camera with smooth zoom and pan, level-of-detail hint, spatial index
   (grid or quadtree) for proximity queries.~~ Done: `render/camera.h`
   (CameraController, editor free camera, `InputFrame::wheel`),
   `core/spatial.h` (SpatialGrid), `camera_test`, `spatial_test`.
4. ~~Editor gizmos: draggable handles, tangent handles, snapping; property
   widgets for curves.~~ Done: `debug/gizmo.h` (Gizmos: point, tangent,
   move, rect, rotate, path; GizmoVisitor), editor scene gizmos with
   click-to-select and snap toolbar, `Widget::Path` inspector widget,
   `App::set_scene_callback`, `gizmo_test`.
5. ~~Generic graph module with weighted edges and shortest-path search;
   steering helpers (follow curve, arrive, keep distance); reflected timers
   and state machines.~~ Done: `sim/graph.h` (Graph, grid, A*/Dijkstra,
   floods), `sim/steering.h` (seek, flee, arrive, pursue, keep_distance,
   separation, turn_toward, PathFollower), `sim/timer.h` (Timer, Cooldown,
   StateMachine), `graph_test`, `gameplay_test`.

All five items are done; this list is complete.

## Active: editor workflow

What makes the editor feel like an editor: an edit mode with Play and Stop,
undo, names, placing things. Generic throughout; the game only supplies
reflected state with hints.

1. ~~Edit and play modes (Play, Pause, Resume, Stop returns to the edited
   state), undo/redo over whole-state snapshots with drag coalescing,
   entity names (first string field) and a filter in the Entities panel,
   right-click placement menu in the scene (create, spawn prefab, move,
   duplicate, delete), Ctrl+Z/Y/D and Del.~~ Done: `debug/edit_history.h`,
   `edit_history_test`, editor.
2. ~~Asset references: an `AssetRef` field type with a hint, a picker in the
   Inspector fed by the asset database, drag from the Assets panel.~~ Done:
   `project/asset_ref.h` (AssetRef = id + path, AssetIndex from sidecars,
   `resolve_asset`), loader overloads on Renderer2D and Audio, Inspector
   picker with type filter and drop target, `asset_ref_test`.
3. ~~Scene management: new scene, scene list, switch scene, unsaved-changes
   marker, save on Ctrl+S; entity hierarchy through an optional parent link.~~
   Done: Scenes panel (new, save, save as, open with unsaved prompt, startup
   scene), dirty marker, Ctrl+S, `sim/hierarchy.h` (Parent, set_parent,
   children_of, destroy_with_children), Entities tree with drag-and-drop
   reparenting, `hierarchy_test`.
4. ~~Editor polish: dark theme with accent, toolbar icons, status bar with
   tick, fps and state, keyboard shortcut reference, welcome screen when
   started without a project.~~ Done: `apply_editor_theme`, coloured
   transport buttons (no icon font: text stays), status bar, Keys panel
   with the editor shortcuts, welcome screen with recent list, native
   folder/file dialogs and New project (`core/system.h`).

This list is complete.

## Active: engine core

Decided 2026-09-06: no 3D yet; the core grows first. Assets, network,
project settings, save games, localisation.

1. ~~Asset pipeline II: audio importer (WAV and Ogg Vorbis to cooked PCM,
   mono downmix setting), shader importer (project shaders/ cooked to
   SPIR-V, loose GLSL compiled on load), import settings edited in the
   Inspector and written to the sidecar, texture thumbnails in the Assets
   panel.~~ Done: `assets/audio_decode.h`, `CookedAudio`,
   `Audio::load_sound`, `compile_glsl_file`, `AssetDatabase::save_meta`,
   `audio_test`.
2. ~~Archives: pack the cooked folder into one `.rwpak` with an index,
   `add_asset_root` accepts a pack, `rewind pack` in the CLI, the runtime
   reads from it transparently.~~ Done: `project/pack.h`, `add_asset_pack`
   and `read_asset_bytes` in `core/paths.h` (all loaders use it), `rewind
   pack`, runner auto-mounts packs and reads settings, scenes and prefabs
   from them, `pack_test`.
3. ~~Project settings II: input bindings (keyboard and gamepad to Button) in
   project.json with a rebinding UI, audio settings (master and per-group
   volume), player settings file (window mode, volume, bindings) saved in
   the user's folder.~~ Done: `core/input_map.h` (InputBindings by name,
   compiled tables, App samples through them), `AudioMix` with sfx/music/ui
   groups on `Audio::play`, `core/player_settings.h` (settings.json in
   SDL's per-user folder, game block), `App::player_settings`, Settings
   section in the overlay and a Player settings panel in the editor with
   press-to-bind, `settings_test`.
4. ~~Core: save games with a version tag and a migration hook, screenshot
   capture, localisation (string tables per language, `tr()`), a settings
   struct games can extend.~~ Done: `sim/savegame.h` (RWSV files with
   version, label, tick; `Game::migrate_save`; `App::save_game/load_game`,
   Saves section in the overlay and editor), `Renderer2D::read_texture` and
   `App::screenshot` (F12, PNG in the user's folder), `core/localization.h`
   (`tr`, `trf`, fallback language, flattened JSON tables), the player
   settings `game` block, `savegame_test`.
5. ~~Network II: sessions with up to four players, lobby with slots, ready
   state and start countdown, the duel sample grows into an arena.~~ Done:
   `AddressedTransport` (UDP to many peers, `LoopbackMesh` for tests),
   per-player acks in `Session`, `net/lobby.h` (host, join, ready, start
   countdown, shared seed, peer list), `App::restart`, the arena sample
   with a lobby screen, three-player and lobby tests.

This list is complete.

## Active: networks and agents

Generic building blocks for anything that moves along connected curves:
roads, rails, rivers, pipes, footpaths, conveyor belts. Nothing in here
knows what a car is. Decided 2026-09-06.

1. ~~Curve network (`sim/network.h`): nodes with elevation, curved edges
   with width and parallel tracks (offset, direction), connectors between
   tracks at nodes with tangent continuity, adjacent-track links, derived
   routing graph with turn costs, conflict pairs at nodes, queries
   (nearest node/edge/track, locate a point on a track), split and merge,
   route planning to a concatenated Path. Reflects; derived data rebuilds.
   Debug drawing in `render/network_draw.h`.~~ Done: `CurveNetwork`,
   `draw_network`, `network_test`.
2. ~~Routing at scale: edge-based costs with dynamic updates and re-route,
   route cache, bidirectional A*.~~ Done: `Graph` penalties and version,
   `bidirectional_path`, `flow_to` (next-edge field for crowds),
   `RouteCache`; `CurveNetwork` track penalties surviving rebuilds,
   `route_affected` for re-planning, built-in cache, `bidirectional` flag.
3. ~~Network agents: follow a track with spacing to the agent ahead (track
   occupancy), change tracks along a transition curve, wait at conflict
   zones and signals (state machines), deterministic and reflected.~~
   Done: `sim/network_agents.h` (NetworkAgents, NetAgent, NetAgentParams,
   entry rule for signals and priority), `agents_test`.
4. ~~Editor tool API: tools a game registers (click, drag, snap, cancel),
   node and track snapping, split and join edges in the scene view.~~
   Done: `debug/editor_tool.h` (EditorTool, ToolContext, NetworkTool),
   `Game::editor_tools`, tool picker and options in the Scene panel,
   `tool_test`; bounce carries a network and the tool.
5. Game UI and time scale: skinned widgets for HUD and menus, pause and
   1x/2x/4x simulation speed.
6. Data tables: definitions in JSON with ids and asset references.
7. Curve fitting for drawn strokes, particles, positional audio, per-draw
   sort keys.

## Later

**3D and isometric 3D tools** (a stated goal): 3D camera and projection,
depth passes, vertex shaders in materials, transform hierarchies, glTF
import, lighting and shadows, 3D picking and gizmos. SDL GPU covers all of
this; NVRHI becomes relevant only for ray tracing, mesh shaders or bindless.

Post-processing and 2D lighting, more than two network players and a
lobby, fixed-point math option, Linux on a real machine, macOS, editor
polish (theme, multi-viewport, thumbnails), positional audio.

Option, not a step: swapping the GPU backend from SDL GPU to NVRHI. Only
`Renderer2D`'s implementation, the App's device and swapchain setup and the
ImGui backend would change; revisit if SDL GPU ever lacks a needed feature
or platform.
