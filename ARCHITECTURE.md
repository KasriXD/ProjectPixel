# Architecture

This describes the system **as it exists now** (v0.1.0). Planned parts are
marked as such. Decisions and their alternatives are recorded in
[docs/adr/](docs/adr/).

## Big picture

```
            ┌───────────────────────────┐        ┌─────────────────────────┐
            │  apps/player (px-player)  │        │ apps/headless           │
            │  desktop client           │        │ (px-headless)           │
            └──┬──────────┬─────────┬───┘        └──────┬──────────────────┘
               │          │         │                   │
     ┌─────────▼───┐ ┌────▼──────┐  │                   │
     │platform_sdl │ │  render   │  │                   │
     │ SDL3 window,│ │ interface,│  │                   │
     │ input, GPU  │─▶ camera,   │  │                   │
     │ backend     │ │ scene draw│  │                   │
     └─────────────┘ └────┬──────┘  │                   │
                          │   ┌─────▼───────────────────▼─────┐
                          │   │ runtime: GameSession          │  no graphics,
                          │   │ fixed ticks, input actions,   │  no window,
                          │   │ gameplay systems              │  no audio
                          │   └──┬──────────────┬─────────────┘
                          │      │              │
                    ┌─────▼──────▼──┐   ┌───────▼───┐   ┌─────────────────┐
                    │ scene (ECS)   │   │ physics   │   │ project formats │
                    └──────┬────────┘   └─────┬─────┘   │ (JSON, versions)│
                           │                  │         └────────┬────────┘
                    ┌──────▼──────────────────▼──────────────────▼───┐
                    │ assets (ids, registry, images)  ─▶  core       │
                    └────────────────────────────────────────────────┘
```

Arrows point from a module to the modules it depends on. Each module is a
static library `px_<name>` (alias `px::<name>`) under `engine/<name>/`, with
public headers in `include/px/<name>/`. CMake target dependencies enforce the
boundaries: for example, `runtime` cannot include SDL headers because it does
not link SDL.

## Modules

| Module | Responsibility | Depends on |
|---|---|---|
| `core` | Logging with sinks, `Result`/`Error`, math (`Vec2`, `Rect`), `Color`, `Guid`, sandboxed file access (`IFileSystem`), `FixedStepClock`, platform-independent `Key` names | std only |
| `assets` | `AssetId` (GUID), `AssetRegistry`, image decoding with size limits | core, stb_image |
| `scene` | Generational entity handles, sparse-set component storage (`Registry`), built-in components, `Scene` (identity, lookup, creation order) | core, assets |
| `physics` | `CollisionWorld`: solid grids and boxes, swept per-axis AABB movement with contact flags | core |
| `project` | Project manifest and scene file formats: parsing, validation, migration, serialization | core, assets, scene, nlohmann/json (private) |
| `runtime` | `GameSession` (one running game), input actions and frames, key bindings, input scripts, gameplay systems | core, scene, physics, project |
| `render` | `IRenderer2D` interface, `Camera2D`, `TextureCache`, scene-to-draw-call extraction, collision debug overlay | core, assets, scene |
| `platform_sdl` | SDL3 window, keyboard events, `SdlRenderer2D` (implements `IRenderer2D`), screenshots | core, render, SDL3 |

Applications compose modules:

- **px-player** = project + runtime + render + platform_sdl. Uses SDL3's main
  callbacks (`SDL_AppInit/Iterate/Event/Quit`) instead of owning a blocking
  loop, which is what browser and iOS builds require.
- **px-headless** = project + runtime only. Proves that games run with no
  graphics; it is the starting point of the dedicated server (Milestone 4).

## Key boundaries

**Engine vs platform services.** Nothing in `engine/` talks to a network
service. A game runs from a local project folder. Platform services
(accounts, catalog, sessions) will be separate programs that talk to the
engine through explicit interfaces (planned: Milestone 6).

**Simulation vs presentation.** `runtime` owns game state and advances only
in fixed ticks from `InputFrame`s. Rendering reads the scene after ticks and
never changes it. This is what lets the same simulation run on a server.

**Client vs server (planned).** Clients will send `InputFrame`s; the server
runs the authoritative `GameSession` and replicates state. Simulation is
already deterministic for identical input on the same build (tested).

**Content vs code.** Games are data: a project folder with a manifest,
scenes and assets. Assets are referenced by GUID, never by path; paths appear
only in the asset registry and are validated to stay inside the project.

**Backends behind interfaces.** SDL types never leave `platform_sdl`.
`render` talks to `IRenderer2D`; `runtime` talks to `CollisionWorld`; project
loading talks to `IFileSystem`.

## Data flow of one frame (px-player)

1. SDL delivers events → `SdlPlatform::processEvent` updates key states.
2. `SDL_AppIterate` → `FixedStepClock` decides how many ticks to run.
3. Per tick: key states → `KeyBindings` → `InputFrame` (held + pressed bits)
   → `GameSession::tick`, which runs, in order: platformer control, kinematic
   movement, collectibles, kill plane, animation, cameras.
4. Render: `cameraForScene` → `renderScene` (layer-sorted, culled, pixel-
   snapped draw calls) → HUD → `SdlRenderer2D` → SDL scales the virtual
   resolution to the window.

## Error handling and logging

- Module boundaries return `Result<T>` / `Status`; no exceptions cross them.
  (nlohmann/json throws internally; `project` converts to `Result`.)
- Error messages are written for creators and name the file and the JSON path,
  e.g. `level1.scene.json: entities[3] ('Player').components.PlatformerController:
  unknown field 'jumpspeed' (did you mean 'jumpSpeed'?)`.
- A missing or corrupt texture is logged once and drawn as a magenta/black
  checkerboard; a game never crashes because of one bad asset.
- `px::log` writes to stderr and to registered sinks (the Studio console will
  be a sink).

## What is deliberately simple for now

| Simplification | Why acceptable now | When it changes |
|---|---|---|
| `SDL_Renderer` backend, no custom shaders | Runs everywhere SDL runs, including WebGL and mobile | Lighting/shader milestone: GPU backend behind `IRenderer2D` (ADR 0003) |
| Kinematic AABB physics only, static geometry built once | Exactly what platformers need; deterministic and testable | Rigid bodies via Box2D v3 behind `physics` (ADR 0004) |
| No entity hierarchy (parent/child) | Not needed by the prototype | Studio foundation, Milestone 3 |
| Animation frames inline in components | Simple first version | Animation clips become assets, Milestone 5 |
| Debug text via SDL's 8x8 font | HUD only | Real text rendering (font assets) before Studio UI work in games |
| Keyboard only | Desktop prototype | Gamepad and touch in Milestone 7 (gamepad may come earlier) |
| Single static component type registry, built-in components only | No scripting yet | Luau components in Milestone 2 |
