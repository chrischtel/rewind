# Roadmap

One list at a time. A list is finished, or parked in a stable state, before
the next one starts.

## Active: project infrastructure

Decided together (2026-09-06): stable asset ids in sidecars, loose cooked
files, fixed project layout, textures and Aseprite cooked first. Done so far:
project file with settings overrides, asset database, content-hash cooking,
`rewind` CLI, editor Assets and Project panels.

Remaining, in order:

1. **Scenes and prefabs.**
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

2. **Entity list and basic property widgets.**
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

## Next: generic engine tools for curve-based top-down games

Engine-level only, never game-specific (no car model, no lane pathfinding).

1. Mesh drawing (triangle lists with texture, UVs, vertex color), polyline
   and ribbon builders with joins, caps and dashes, draw layers and sort keys.
2. Curves: Bezier and Catmull-Rom, arcs, arc length, tangents, offset,
   split and join, closest point. Polygon triangulation and offset.
3. Camera with smooth zoom and pan, level-of-detail hint, spatial index
   (grid or quadtree) for proximity queries.
4. Editor gizmos: draggable handles, tangent handles, snapping; property
   widgets for curves.
5. Generic graph module with weighted edges and shortest-path search;
   steering helpers (follow curve, arrive, keep distance); reflected timers
   and state machines.

## Later

Post-processing and 2D lighting, more than two network players and a
lobby, fixed-point math option, Linux on a real machine, macOS, editor
polish (theme, multi-viewport, thumbnails), positional audio.

Option, not a step: swapping the GPU backend from SDL GPU to NVRHI. Only
`Renderer2D`'s implementation, the App's device and swapchain setup and the
ImGui backend would change; revisit if SDL GPU ever lacks a needed feature
or platform.
