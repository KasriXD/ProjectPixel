# Engine guide

How the engine works today. For module boundaries see
[ARCHITECTURE.md](../ARCHITECTURE.md).

## Coordinates and units

- World units: 1 unit = 1 pixel at camera zoom 1. Game logic never deals in
  screen pixels.
- Y points down, matching screen and tilemap row order. Gravity is positive.
- Angles are degrees, clockwise.
- The game renders at a **virtual resolution** set by the project (320x180 in
  the example). The platform layer scales it to the window: whole-number
  scaling with borders (`integer`, crisp pixel art) or aspect-preserving
  fractional scaling (`letterbox`, better for high-resolution art).
- The camera's view origin is snapped to whole screen pixels and sprites and
  tiles are drawn at rounded positions, so pixel art does not shimmer.
  Simulation positions stay fractional.

## Scene model

- **Entity**: a generational handle (`index`, `generation`). Destroyed
  entities' handles become invalid even if the slot is reused.
- **Identity**: every entity has an `EntityInfo` with a GUID and a name. The
  GUID is what files, references (e.g. `Camera.follow`) and, later, network
  replication use. Handles are runtime-only.
- **Components**: plain structs stored per type in dense arrays (sparse sets).
  `Registry::each<A, B>(fn)` visits entities that have all listed components.
  A callback may destroy the entity it is visiting.
- **Hierarchy**: an entity may have a parent. The scene keeps its entities
  in depth-first order (each parent before its children). A `Transform` is
  relative to the parent's; world transforms are computed by composing the
  ancestors' transforms, and entities without a `Transform` (groups) do not
  move their children. Destroying an entity destroys its children. Physics,
  triggers, cameras and rendering work in world space
  ([ADR 0016](adr/0016-scene-hierarchy.md)).
- **Scene**: registry + settings + id index + hierarchy order. Scenes are
  copyable; copies are fully independent (used for restart, Studio's undo
  history and play mode).
- **Component descriptors** describe every component and field (type, range,
  description). The file format, validation, Studio's Properties panel and
  [components.md](components.md) are generated from them
  ([ADR 0014](adr/0014-component-descriptors.md)).

Component reference: [components.md](components.md).

## Simulation

`runtime::GameSession` runs one game. It is created with
`GameSession::create` (or `runtime::startSession` for a loaded project),
which fails with a clear error if the scene's scripts are invalid. It
advances only through `tick(InputFrame)`, one fixed step of `1 / tickRate`
seconds. Each tick runs, in order:

1. **Scripts**: `Start` for new script instances, then `Tick`
   ([scripting.md](scripting.md#execution-order)).
2. **Moving solids**: solid colliders are re-read every tick (scripts may
   move them); bodies standing on a solid that moved are carried by the same
   offset, through collision.
3. **Character control**: `PlatformerController` reads actions, applies
   acceleration, gravity, jump (with coyote time and jump buffering) and jump
   cut, and flips the sprite. Then `TopDownController` moves in eight
   directions without gravity (`move_left/right/up/down`; diagonals are
   normalised so they are not faster), with separate acceleration and
   deceleration per axis, remembers the facing direction and flips the
   sprite when walking left.
4. **Kinematic movement**: moves each body by `velocity * dt` through the
   collision world, X then Y; sets ground/ceiling/wall contacts, records
   which solid entity it stands on, and zeroes velocity into walls.
5. **Triggers**: overlaps between trigger colliders and moving bodies are
   compared with the previous tick; `TriggerExit`/`TriggerEnter` go to the
   scripts of both entities.
6. **Kill plane**: respawns players below `killPlaneY` at their spawn point
   (`PlayerRespawned`); scripts respawn players the same way with
   `entity:respawn()`.
7. **Animation**: advances `SpriteAnimation` and updates `Sprite.source`.
8. **Cameras**: follow targets, clamp to level bounds.

After the script and trigger phases, entities destroyed by scripts are
removed and queued script events are delivered. Game rules such as
collecting coins and winning are scripts; the session keeps the
authoritative `GameState` (score, objective, completion, script errors)
and reports changes as `events()` of the last tick.

The client converts real time to ticks with `FixedStepClock` (at most 8 ticks
per frame; a longer stall drops time instead of freezing). Given the same
build, starting scene and input frames, a session produces bit-identical
results; this is covered by a test.

### Input

Gameplay sees **actions** (`jump`), never keys. The project lists actions and
default keys. Each tick receives an `InputFrame` with two bitmasks: actions
held, and actions pressed since the previous tick. The client sets "pressed"
for keys tapped between two ticks, so very short taps are not lost. The same
frames are what a client will send to a multiplayer server.

## Collision

`physics::CollisionWorld` holds static solids: grids (from solid tilemaps)
and boxes (solid colliders without a `KinematicBody`). `moveBox` sweeps an
axis-aligned box along X then Y against every solid in the swept area, so
fast objects cannot pass through thin walls. Surfaces that touch exactly
count as contacts; boxes already overlapping a solid can always move out.
Static geometry is gathered once when the session starts.

Triggers are overlap tests between colliders (currently players against
collectibles).

## Rendering

- `render::IRenderer2D` is the backend interface: textures, sprites
  (with source rect, rotation, flips, tint), filled and outlined rectangles,
  debug text, frame begin/end. Coordinates are virtual-resolution pixels.
- `render::renderScene` draws tilemaps and sprites sorted by layer (stable
  within a layer: tilemaps, then sprites in creation order), skipping
  off-screen sprites and tiles.
- `render::TextureCache` loads texture assets on first use. Failures are
  logged once and drawn as a magenta/black checkerboard.
- The SDL backend (`platform_sdl::SdlRenderer2D`) uses `SDL_Renderer`, which
  chooses Direct3D, Metal, Vulkan, OpenGL or a software renderer per platform
  and batches draw calls.

## Logging

`px::log::{trace,debug,info,warn,error}` with `std::format` syntax. Output goes
to stderr and to sinks registered with `log::addSink` (sinks must not log).
`--verbose` on the apps enables debug messages.
