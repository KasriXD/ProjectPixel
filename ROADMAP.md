# Roadmap

Status key: **Done** (implemented and verified), **Partial**, **Planned**.
"Verified" always names where it was verified.

## Current focus

Milestones 0-4 are done: the multiplayer prototype (dedicated server, UDP
transport, players joining and leaving, snapshot replication with
interpolation and prediction, Studio's local multiplayer test) works and is
tested end to end. CI builds and tests on Linux, Windows (MSVC), macOS and
(experimentally) WebAssembly. Recommended next: two or three people play a
multiplayer test over a real network and creators build small levels in
Studio; then Milestone 5 (see "Recommended adjustments" below).

## Blockers and open items

| Item | Impact | Resolution |
|---|---|---|
| Not yet run on real macOS hardware (CI passes) | Rendering/input on Macs unconfirmed | A Mac check when one is available. Windows: played on a Windows 11 PC (2026-10-23) |
| Gamepad input not implemented | Keyboard only | Small change in `platform_sdl` + bindings; scheduled with Milestone 7, could move earlier |
| Browser build not yet tried in a real desktop browser by a person | Input feel, audio policies, page integration unconfirmed | Founder to open the CI-built `px-player-web` artifact (serve it with any static web server) |
| Multiplayer played only on one computer (separate processes over real UDP) and in simulated networks so far | Behaviour over Wi-Fi and the internet (latency spikes, NAT, firewalls) unconfirmed | Founder runs Studio's *Test Multiplayer*, then `px-server` on one PC and `px-player --connect` from another on the same network |
| Studio used only by its developers and the founder (Windows 11) so far | Usability problems for new creators unknown | Two or three creators build a small level and report friction; the Toolbox tutorial in docs/studio.md is the script |

---

## Milestone 0: Repository and engineering foundation (**Done**)

- [x] Environment inspection (Ubuntu 24.04, GCC 13, Clang 18, CMake 3.28, Ninja)
- [x] Repository structure, module layout, CMake presets
- [x] C++20, warnings (and warnings-as-errors preset), sanitizer preset
- [x] Dependency management: pinned + hashed downloads, vendored single headers ([ADR 0002](docs/adr/0002-dependency-management.md))
- [x] Application entry points (`px-player`, `px-headless`)
- [x] Automated tests (doctest + CTest)
- [x] Logging with sinks; `Result`/`Error` error handling
- [x] Build documentation (README, CONTRIBUTING)
- [x] CI on GitHub Actions: Linux GCC/Clang/ASan+UBSan/headless, Windows MSVC, macOS; all passing

## Milestone 1: Engine prototype 0.1 (**Done** on Linux)

- [x] Window creation and application loop (SDL3 main callbacks)
- [x] Keyboard input mapped to named actions; taps shorter than a tick are not lost
- [x] 2D rendering foundation behind `IRenderer2D`; SDL_Renderer backend
- [x] Texture loading (PNG/JPEG/BMP/TGA) with size limits; missing-texture placeholder
- [x] Sprites, sprite animation, tilemaps, layers, pixel-perfect integer scaling
- [x] Camera with follow and level-bounds clamping
- [x] Scene representation and entity/component system with stable ids
- [x] Collision: solid tiles and boxes, triggers; no tunnelling
- [x] Platformer controller: acceleration, variable jump, coyote time, jump buffering
- [x] Collectibles, kill plane and respawn, level-complete state, HUD
- [x] Versioned project and scene formats with strict validation and migration hook
- [x] Example platformer loaded entirely from project files
- [x] Tests: 51 unit test cases; scripted full playthrough; windowless client smoke test
- [x] Verified visually on Linux: X11 + OpenGL (Mesa) and offscreen + software renderer
- [x] Windows build (MinGW-w64) runs under Wine: tests, playthrough, client with Direct3D renderer
- [x] Clean under AddressSanitizer + UBSan (leak detection on); Release builds clean with `-Werror`

Deliverable met: a playable 2D game loaded from project files with a
controllable character, solid platforms and collectible objects.

## Milestone 2: Gameplay and scripting (**Done**)

- [x] Luau 0.743 vendored (Common, Ast, Bytecode, Compiler, VM) with license,
      pinned commit and update script ([ADR 0009](docs/adr/0009-luau-integration.md))
- [x] `px_scripting` module: one sandboxed VM per session, read-only globals,
      per-script environments, no OS/file/process/debug access, deterministic `math.random`
- [x] Limits: memory cap (allocator), deterministic CPU step budget per
      callback plus wall-clock watchdog, event and log limits; restricted
      operations tested as blocked
- [x] `Script` and `Tags` components; script assets; scene format 2 with a
      migration for the removed `Collectible` component
- [x] Lifecycle `Start`, `Tick`, `TriggerEnter`, `TriggerExit`, `Destroy`;
      documented execution order, deferred destroy, per-instance fail-stop
      ([ADR 0010](docs/adr/0010-script-execution-model.md))
- [x] Gameplay API v1: `Game`, `World`, `Input`, `Events`, `Entity`, `Vec2`;
      one binding table generates the reference ([docs/scripting-api.md](docs/scripting-api.md))
- [x] Realms: authority-only API absent from client VMs, tested ([ADR 0011](docs/adr/0011-script-realms.md))
- [x] Errors with file, line, entity and event, in the log and on screen
- [x] Example: coins, win condition and a moving platform are Luau scripts;
      moving solids carry riders
- [x] Hot reload (F5) of scripts and scenes in `px-player`
- [x] Visual-scripting design ([docs/visual-scripting.md](docs/visual-scripting.md))
- [x] Tests: 24 new test cases (75 total); all CI configurations pass
- [x] Browser feasibility: WebAssembly build of engine, Luau and SDL3 in CI
      ([ADR 0012](docs/adr/0012-browser-runtime.md))

Deliverable met: game behaviour is customised by Luau scripts loaded from
project files, without recompiling the engine.

Follow-ups (not blocking): multiple scripts per entity, `require` for shared
modules, spawning entities from scripts, structured parameter metadata for
visual nodes, per-tick total CPU budget for servers, trigger broadphase.

## Milestone 3: Native Studio foundation (**Done**)

- [x] Scene hierarchy: parent links, depth-first order, local transforms with
      computed world transforms, subtree copy/destroy, cycle and depth checks;
      physics, rendering, cameras and scripts work in world space
      ([ADR 0016](docs/adr/0016-scene-hierarchy.md))
- [x] Component descriptors drive the file format, validation, the inspector
      and the generated [component reference](docs/components.md)
      ([ADR 0014](docs/adr/0014-component-descriptors.md))
- [x] Multiple behaviours per entity (`Scripts` list); scene format 3 with
      migration from 2; scripts can read and change the hierarchy
- [x] `px_editor`: UI-free editor model with projects, assets, scene
      documents, snapshot undo/redo, selection by id, picking, clipboard and
      play mode ([ADR 0015](docs/adr/0015-studio-architecture.md))
- [x] Studio (`px-studio`, Dear ImGui docking, [ADR 0013](docs/adr/0013-studio-ui-dear-imgui.md)):
      project creation (empty or template) and opening, recent projects
- [x] Explorer panel (entity tree): drag-and-drop parenting and reordering, rename,
      create presets, duplicate, copy/paste as text (also between projects that share assets), delete, search
- [x] Scene viewport: real renderer, pan/zoom, grid and snapping, click and
      box selection, move with undo, tile paint/erase/pick, collider and
      camera overlays, drag-and-drop of images, scripts and scenes
- [x] Properties panel generated from descriptors; add/remove components with
      requirements; scripts with typed properties and reset; live values while playing
- [x] Asset browser: import (dialog, typed path or drop), new script/scene,
      rename, find uses, delete with usage check
- [x] Script editor with live compile feedback; external changes detected
- [x] Save/load with validation and canonical files; unsaved-changes prompts
- [x] Play/pause/step/stop on a copy of the scene with the real `GameSession`
- [x] Output panel (log sink) with filters, repeat folding, open-at-line for script errors
- [x] Tests: 28 new test cases (103 total), a Studio smoke test in CTest;
      the editor model builds and plays a game from an empty project in a test
- [x] Verified: a game built in Studio from an empty project (import, paint,
      player, coins with a new script, save, play) runs unchanged in
      `px-player` and `px-headless`; Studio runs under Wine (Direct3D) and in
      CI on Windows (MSVC) and macOS
- [ ] Prefabs: deferred to Milestone 5 together with spawning entities from
      scripts (they need the same "instantiate an entity template" mechanism)

Deliverable met: a creator can build and save a simple game using the editor
rather than manually editing project files ([walkthrough](docs/studio.md#tutorial-your-own-art-and-behaviour)).

Known limitations: one entity at a time in Properties, whole-scene undo
snapshots, plain-text script editor, rotation/scale ignored by colliders and
tilemaps ([details](docs/studio.md#limitations)).

### Studio hardening (after Milestone 3, **in progress**)

Driven by the founder's first session on Windows 11:

- [x] Crash when clicking *Stop* fixed; play and project changes now happen
      between frames
- [x] Standard layout familiar to Roblox Studio users: Explorer above
      Properties on the right, Assets on the left, Output below the scene;
      the previous arrangement remains as *View > Layout > Classic*
- [x] `windows` CMake preset that uses the newest installed Visual Studio or
      Build Tools (2022 or 2026); Windows build steps in the README
- [x] New projects go into the folder used last time (remembered per user)
- [x] Game types chosen on the welcome screen: **platformer** and
      **top-down** (new `TopDownController`, 8-direction movement), each with
      an example level, default input and player; project format 2 stores
      the choice. More kinds (e.g. racing, puzzle grid) need a controller,
      art and library entries each
- [x] **Toolbox** panel: a starter library of 32 images and behaviours and 28
      ready-made objects (heroes, ground and walls, coins, gems, keys and
      locks, hazards, enemies, springs, moving platforms, checkpoints,
      exits, signs, scenery) that can be clicked or dragged into any
      project, empty ones included ([format](docs/library-format.md)); new
      scripting API `entity:respawn()`, `entity.spawnPoint`,
      `Game.getObjective()`
- [ ] A toolbar with large Move/Rotate/Scale, Insert and Play buttons;
      rotate and scale handles in the Scene view
- [ ] Editing several selected entities at once in Properties
- [ ] Animation frame picker for sprite sheets
- [ ] Let creators add their own items to the Toolbox (their saved objects;
      later the marketplace of Milestone 8)
- [ ] Feedback from two or three more creators

## Milestone 4: Multiplayer prototype (**Done**)

Verified on Linux (server and player windows as separate processes over
UDP, on one computer), with a Windows build (MinGW, under Wine) joining a
Linux server (the top-down level played to the end through the network,
with no prediction corrections; a Linux and a Windows window in one game),
and by end-to-end tests in CI on Linux, Windows and macOS. Guide: [docs/multiplayer.md](docs/multiplayer.md); design:
[ADR 0018](docs/adr/0018-multiplayer-networking.md).

- [x] Transport chosen and behind an interface: UDP via ENet 1.3.18
      (vendored), plus an in-process loopback network with simulated
      latency, jitter, loss and duplication; WebSocket/WebTransport for
      browsers fits the same interface later
- [x] Wire protocol v1 (hello/welcome/reject, input, snapshots, goodbye)
      with strict decoders, fuzzed under AddressSanitizer
- [x] Dedicated server `px-server` built on the headless runtime; listen
      server in `px-player --host`
- [x] Session lifecycle: version and game-copy checks, player limit, hello
      timeout, leaving and disconnects, level restart after completion
- [x] Player spawning: the scene's Player entities are templates and spawn
      points; deterministic ids; players sharing a spawn stand side by side
- [x] Server-authoritative `GameSession` with per-player input buffers
      (jitter absorption, catch-up)
- [x] Snapshot replication: generic over the component descriptors,
      per-field deltas against each client's acknowledged state
- [x] Clients: interpolation of other entities; prediction and smoothed
      reconciliation of the own character with the server's movement code
- [x] Scripting API for players: `PlayerJoined`/`PlayerLeft`, `Players`,
      `entity.playerName/playerId`, per-player `Input`
- [x] Local multi-client test tools: Studio's *Test Multiplayer* (server +
      1-4 windows, output in Studio), `px-headless --net-test` (N clients,
      simulated network, report) and `--connect` (bots)

Next for multiplayer (not part of the prototype):

- [ ] Client-side behaviours (presentation) and client-to-server requests
      validated by server scripts (like Roblox RemoteEvents)
- [ ] Per-player state in the API (score, inventory) and team support
- [ ] Connect tokens from the platform, encryption, rate limits (Milestone 6)
- [ ] Interest management for large worlds; lag compensation for shooting
- [ ] A browser transport (WebSocket or WebTransport) for web clients

## Milestone 5: Visual scripting and expanded engine (**Planned**)

Typed node graphs compiled to the same gameplay API as Luau; graph editor;
prefabs and spawning entities from scripts; tilemap and animation editors;
animation clip assets; tileset assets with per-tile properties (one-way
platforms, slopes).

## Milestone 6: Platform backend and publishing (**Planned**)

Accounts, profiles, catalog, publishing and versioning, asset storage,
sessions and matchmaking, persistent data, friends, moderation tools.

## Milestone 7: Cross-platform clients (**Planned**)

Android, iOS, browser runtime (Emscripten + WebGL/WebGPU), touch and gamepad,
per-client performance settings, cross-platform multiplayer.

## Milestone 8: Creator ecosystem and scaling (**Planned**)

Persistent worlds, social hubs, collaboration, marketplace, analytics,
monetization, advanced moderation, scalable hosting.

---

## Recommended adjustments

1. **Next: real-world multiplayer tests, then Milestone 5.** Play a
   multiplayer test on two or three computers over a home network and over
   the internet, and let creators build small co-op levels; fix what they
   hit (likely: per-player score, client-to-server requests, the multi-entity
   Properties panel and an animation frame picker). Prefabs and spawning
   from scripts (Milestone 5) should replicate through the same snapshot
   path, which already sends entities created at run time.
2. **Prefabs with spawning in Milestone 5**: both need the same
   "instantiate a saved entity template" mechanism, which player templates
   now use; spawned entities replicate like characters do.
3. **Keep the WebAssembly job in CI, but defer the browser runtime itself**
   (ADR 0012): it already builds and runs, so regressions are caught for
   free; productising it (loading projects over the network, asset
   streaming, page integration, size budgets) belongs with the platform
   work in Milestones 6-7.
4. **Add gamepad support early** (small, and platformers need it to feel right).
5. **Keep the simulation deterministic** (fixed ticks, input frames, no
   wall-clock reads in gameplay). It is already tested and makes replays,
   automated playtests and server reconciliation much easier.
