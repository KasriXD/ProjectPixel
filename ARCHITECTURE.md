# Architecture

This describes the system **as it exists now** (v0.3.0). Planned parts are
marked as such. Decisions and their alternatives are recorded in
[docs/adr/](docs/adr/).

## Big picture

```
  ┌──────────────────────────┐
  │ apps/studio (px-studio)  │  Dear ImGui panels (UI only)
  └────┬─────────────────────┘
       │ uses everything px-player uses, plus
  ┌────▼─────────────────────┐
  │ editor: projects, scene  │  no UI: undo, selection, picking,
  │ documents, play mode     │  assets, clipboard (unit tested)
  └──────────────────────────┘

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
                          │   │ built-in systems, IScriptHost │  no audio
                          │   └──┬─────────┬──────────┬───────┘
                          │      │         │          │
                          │      │  ┌──────▼───────┐  │
                          │      │  │ scripting    │  │   Luau VM (vendored),
                          │      │  │ sandbox,     │  │   sandbox, limits,
                          │      │  │ API, Script  │  │   gameplay API
                          │      │  │ lifecycle    │  │
                          │      │  └──────┬───────┘  │
                    ┌─────▼──────▼──┐◀─────┘  ┌───────▼───┐   ┌─────────────────┐
                    │ scene (ECS)   │         │ physics   │   │ project formats │
                    └──────┬────────┘         └─────┬─────┘   │ (JSON, versions)│
                           │                        │         └────────┬────────┘
                    ┌──────▼────────────────────────▼──────────────────▼───┐
                    │ assets (ids, registry, images)  ─▶  core             │
                    └──────────────────────────────────────────────────────┘
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
| `scene` | Generational entity handles, sparse-set component storage (`Registry`), built-in components, `Scene` (identity, lookup, parent/child hierarchy, world transforms), component descriptors (`ComponentInfo`: fields, ranges, requirements, validation) | core, assets |
| `physics` | `CollisionWorld`: solid grids and boxes, swept per-axis AABB movement with contact flags | core |
| `project` | Project manifest and scene file formats: parsing, validation, migration, canonical serialization (generic over component descriptors); `px.entities` clipboard documents; the starter library manifest (`px.library`) | core, assets, scene, nlohmann/json (private) |
| `scripting` | `ScriptLibrary` (compiles Luau at load), `ScriptVm` (sandbox, memory and CPU limits), `ScriptSystem` (Script instances, lifecycle, events), gameplay API bindings and their machine-readable reference | core, assets, scene, Luau (private) |
| `runtime` | `GameSession` (one running game; implements `IScriptHost`), `loadGame`/`startSession`, input actions and frames, key bindings, input scripts, built-in systems (platformer and top-down control, movement, moving solids, triggers, respawning) | core, scene, physics, project, scripting |
| `render` | `IRenderer2D` interface, `Camera2D`, `TextureCache`, scene-to-draw-call extraction, collision debug overlay | core, assets, scene |
| `platform_sdl` | SDL3 window, keyboard events, `SdlRenderer2D` (implements `IRenderer2D`, render targets), screenshots | core, render, SDL3 |
| `editor` | The editor model without UI: `EditorProject` (create/open by game type, assets, scenes, script buffers), `SceneDocument` (operations, selection, snapshot undo), picking, `PlayMode`, `LogBuffer`, `Library` (loads and validates the starter library; inserts its objects and files into a project) | core, assets, scene, project, scripting, runtime |

Applications compose modules:

- **px-player** = project + runtime + render + platform_sdl. Uses SDL3's main
  callbacks (`SDL_AppInit/Iterate/Event/Quit`) instead of owning a blocking
  loop, which is what browser and iOS builds require.
- **px-headless** = project + runtime only. Proves that games run with no
  graphics; it is the starting point of the dedicated server (Milestone 4).
- **px-studio** = editor + render + platform_sdl + Dear ImGui. Panels draw
  and call `px_editor`; they hold only UI state, so the UI toolkit can be
  replaced without touching editing logic ([ADR 0013](docs/adr/0013-studio-ui-dear-imgui.md),
  [ADR 0015](docs/adr/0015-studio-architecture.md)). Desktop only; games never
  link ImGui.

## Key boundaries

**Engine vs platform services.** Nothing in `engine/` talks to a network
service. A game runs from a local project folder. Platform services
(accounts, catalog, sessions) will be separate programs that talk to the
engine through explicit interfaces (planned: Milestone 6).

**Scripting vs runtime.** `scripting` does not depend on `runtime`. Every
script action that changes game state goes through `IScriptHost`, which
`GameSession` implements. That seam is where a dedicated server will
validate and replicate changes, and why scripts cannot reach anything the
host does not offer. Scripts are content: they are compiled from the
project when it loads and never linked into the engine.

**Authority (realms).** Script VMs run in the *authority* realm (local
session now, server later) or the *client* realm. Authority-only API
members do not exist in client VMs (ADR 0011).

**Simulation vs presentation.** `runtime` owns game state and advances only
in fixed ticks from `InputFrame`s. Rendering reads the scene after ticks and
never changes it. This is what lets the same simulation run on a server.

**Client vs server (planned).** Clients will send `InputFrame`s; the server
runs the authoritative `GameSession` and replicates state. Simulation is
already deterministic for identical input on the same build (tested).

**Content vs code.** Games are data: a project folder with a manifest,
scenes and assets. Assets are referenced by GUID, never by path; paths appear
only in the asset registry and are validated to stay inside the project. The
starter library behind Studio's Toolbox is content too (`library/`, generated
by `tools/`): inserting an item copies its files into the project with the
library's GUIDs, so a project never depends on the library at run time and
repeated inserts reuse the same assets ([library-format.md](docs/library-format.md)).

**Editor vs engine.** `editor` may use any engine module but no UI or SDL;
engine modules never depend on `editor`. Studio plays games with the same
`GameSession` as `px-player`, on a copy of the edited scene.

**Backends behind interfaces.** SDL types never leave `platform_sdl`.
`render` talks to `IRenderer2D`; `runtime` talks to `CollisionWorld`; project
loading talks to `IFileSystem`.

## Data flow of one frame (px-player)

1. SDL delivers events → `SdlPlatform::processEvent` updates key states.
2. `SDL_AppIterate` → `FixedStepClock` decides how many ticks to run.
3. Per tick: key states → `KeyBindings` → `InputFrame` (held + pressed bits)
   → `GameSession::tick`, which runs, in order: script `Start`/`Tick`,
   moving solids, platformer control, kinematic movement, trigger events to
   scripts, kill plane, animation, cameras. Destroys and script events are
   flushed after each script phase (docs/scripting.md#execution-order).
4. Render: `cameraForScene` → `renderScene` (layer-sorted, culled, pixel-
   snapped draw calls) → HUD (objective and messages set by scripts, script
   errors) → `SdlRenderer2D` → SDL scales the virtual resolution to the window.

Loading: `runtime::loadGame` reads `project.json`, the start scene and every
script (compiling them, so syntax errors surface before play), producing an
immutable `LoadedGame`. `startSession` creates a `GameSession` from it;
restart (R) starts a new session, reload (F5) loads a new `LoadedGame`.

## Error handling and logging

- Module boundaries return `Result<T>` / `Status`; no exceptions cross them.
  (nlohmann/json throws internally; `project` converts to `Result`.)
- Error messages are written for creators and name the file and the JSON path,
  e.g. `level1.scene.json: entities[3] ('Player').components.PlatformerController:
  unknown field 'jumpspeed' (did you mean 'jumpSpeed'?)`.
- A missing or corrupt texture is logged once and drawn as a magenta/black
  checkerboard; a game never crashes because of one bad asset.
- `px::log` writes to stderr and to registered sinks (Studio's Output panel is a
  `LogBuffer` sink).

## What is deliberately simple for now

| Simplification | Why acceptable now | When it changes |
|---|---|---|
| `SDL_Renderer` backend, no custom shaders | Runs everywhere SDL runs, including WebGL and mobile | Lighting/shader milestone: GPU backend behind `IRenderer2D` (ADR 0003) |
| Kinematic AABB physics only; moving solids carry riders but do not push | Exactly what platformers need; deterministic and testable | Rigid bodies via Box2D v3 behind `physics` (ADR 0004) |
| Trigger detection is all triggers x all bodies each tick | Small scenes | Spatial broadphase before large worlds |
| World transforms computed on demand (no cache) | Shallow hierarchies, small scenes | Cache with dirty flags when profiling shows deep hierarchies matter |
| Studio undo stores whole-scene snapshots | Correct by construction; ~1.7 ms and 3.7 MB per step at 10,000 entities | Diff-based snapshots for large worlds (ADR 0015) |
| Animation frames inline in components | Simple first version | Animation clips become assets, Milestone 5 |
| Debug text via SDL's 8x8 font | HUD only | Real text rendering (font assets) before Studio UI work in games |
| Keyboard only | Desktop prototype | Gamepad and touch in Milestone 7 (gamepad may come earlier) |
| Built-in component types only; scripts reach components through a fixed API | Keeps the API reviewable and versioned | Descriptors exist (ADR 0014); exposing them to scripts is an API decision for later |
| No prefabs; no `require`; scripts cannot spawn entities | Copy/paste and duplicate cover reuse in one scene | Prefabs and spawning together (Milestone 5) |
| One script VM per session, limits per callback | Single-player | Per-tick total budgets and isolated server processes (Milestones 4/6) |
