# Style

How the game looks and feels, written down so it can be followed by someone
who cannot draw. Every rule here is a number or a constraint, not a taste.

The goal is *hochwertig*: smooth, clean, direct. That is not draughtsmanship.
It is restraint, consistent measurement, good type and motion that never
snaps. All four are programming.

The reference is Mini Motorways and Dorfromantik: no art, only shapes, and
they look like products.

## The rules that never bend

1. **No hand-drawn art.** Every surface is geometry, a shader, or type.
2. **Nothing snaps.** Every value the eye can follow moves over time.
3. **Everything sits on the grid.** 8 units in the UI, whole metres in the
   world.
4. **Nine colours, and no tenth.** Listed below.
5. **One type family, two weights, plus mono for numbers.**
6. **No gradients, no bevels, no drop shadows in the UI.** Depth in the
   world only, and only from light.

When something looks wrong, the fix is almost always to remove a colour, a
border or a shadow — not to add one.

## Colour

Paper ground, ink lines, one warm signal colour. Day palette:

| Token | Hex | Used for |
| --- | --- | --- |
| `ground` | `#EFEBE3` | the world the yard sits on |
| `surface` | `#F8F6F1` | panels, cards, the forecourt |
| `line` | `#C4BFB5` | rules, borders, the grid, hairlines |
| `ink` | `#22272B` | text, outlines, vehicle bodies |
| `muted` | `#6B7075` | secondary text, disabled, distant detail |
| `accent` | `#CE5A28` | the one warm colour: selection, the player's own |
| `good` | `#4A7C59` | on time, in profit, loaded |
| `warn` | `#C9A227` | late, low, wearing |
| `bad` | `#A63D2F` | failed, empty, overdue |

`ground`, `surface` and `line` are one family and read as one colour, which
is why this list is really five.

Night is the same palette with `ground` and `ink` swapped in role and
everything desaturated one step — not a second palette to maintain.

**Cargo colours are not free.** A cargo type gets `accent`, `good`, `warn`
or `muted` and nothing else. If more are needed, the answer is a label.

## Type

**IBM Plex Sans** for everything, **IBM Plex Mono** for numbers in tables
and readouts. Both are free under the OFL, both are drawn for technical and
industrial work, and the mono has tabular figures so a column of tonnages
lines up and stops jittering as it counts.

| Role | Face | Size |
| --- | --- | --- |
| Panel title | Plex Sans Medium | 16 |
| Body | Plex Sans Regular | 14 |
| Secondary | Plex Sans Regular, `muted` | 12 |
| Number, table, readout | Plex Mono Regular | 14 |
| Big figure (money, clock) | Plex Mono Light | 28 |

Uppercase only for section labels, at 11 with 0.08 em tracking. Never for
sentences.

The single largest jump in perceived quality available here is replacing a
default font with a real one and putting every number in mono. Do it in
phase 1.

## Motion

Nothing appears, disappears or jumps. Everything eases.

| Thing | Duration | Curve |
| --- | --- | --- |
| Hover, press | 120 ms | ease-out |
| Panel open, tab change | 180 ms | ease-out |
| A number changing | 240 ms | ease-out, counted not cut |
| Camera pan and zoom | 300 ms | ease-out, already in `CameraController` |
| A vehicle appearing | 240 ms | ease-out on scale and opacity |

Ease-out is `1 - (1-t)³`. That one curve covers everything on this list;
resist adding a second until something genuinely needs it.

This is what `core/ease.h` is for, and why it is a phase 1 engine item: a
reflected `Tweened<float>` that every UI value goes through means motion is
correct by default rather than by discipline.

## Measure

**UI.** An 8 unit grid. Padding 16, gap between items 8, gap between groups
24. Rows 32 high, buttons 32, corner radius 4 and only 4. Panels are
`surface` on `ground` with a 1 unit `line` border and no shadow.

**World.** Real metres, like the traffic game. Track gauge 1.435 m, a lane
3.25 m, a wagon 14 m, a lorry 12 m. Getting these right is free and it is
most of why a top-down scene reads as convincing.

**Line weights,** in world metres so they scale with zoom:

| Weight | Metres | Used for |
| --- | --- | --- |
| Hairline | 0.08 | grid, hatching, secondary markings |
| Standard | 0.14 | outlines, rails, edge lines |
| Emphasis | 0.30 | selection, the route being planned |

## The world

- **Ground** is a shader over a hash, like the traffic game's grass: worn
  ballast, hardstanding, grass, gravel. No textures, no tiling artefacts.
- **Vehicles** are geometry from above: the silhouette, the glass, one
  accent stripe. Proportions must be right; detail must not be attempted.
- **Buildings** are extruded footprints with a flat roof plane and a single
  soft shadow. A workshop is a shape with a door on the correct side.
- **Contrast is low.** The world sits between `ground` and `line`; only the
  player's own things and the state colours are allowed to be saturated.
- **Density is the enemy.** At far zoom, drop everything but the network
  and the vehicles. `CameraController` already has the level-of-detail hint.

## Sound

Cheap, and it carries more of *hochwertig* than any pixel on this page.

- One soft click for a press, one duller one for a toggle. No beeps.
- A distant diesel that changes with load — the same `F` from the motion
  model drives the pitch, so the engine note rises on the grade for free.
- Rain on the yard, wind in the valley, a level crossing bell.
- Silence is allowed. The default state is quiet.

## Where the polish actually lands

In order of quality gained per hour spent:

1. The font, and mono numbers. *(phase 1)*
2. Easing on everything. *(phase 1)*
3. The palette held to nine colours. *(phase 1)*
4. Correct real-world proportions in the world. *(phase 2)*
5. Sound. *(phase 4–5)*
6. 2D lighting, soft shadows, vignette, grading. *(phase 5)*

The first three are the cheapest and the largest, they are all code, and
they are all in phase 1 for that reason.
