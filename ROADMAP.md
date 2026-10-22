# Roadmap

Status key: **Done** (implemented and verified), **Partial**, **Planned**.
"Verified" always names where it was verified.

## Current focus

Milestones 0, 1 and 2 are done. CI builds and tests on Linux, Windows (MSVC),
macOS and (experimentally) WebAssembly. Recommended next: Milestone 3, the
Native Studio foundation (see "Recommended adjustments" below).

## Blockers and open items

| Item | Impact | Resolution |
|---|---|---|
| Game not yet played interactively on real Windows or macOS hardware (CI passes on both) | Rendering/input on real GPUs unconfirmed | Founder to try the Windows package on a real PC; a Mac check when one is available |
| Gamepad input not implemented | Keyboard only | Small change in `platform_sdl` + bindings; scheduled with Milestone 7, could move earlier |
| Browser build not yet tried in a real desktop browser by a person | Input feel, audio policies, page integration unconfirmed | Founder to open the CI-built `px-player-web` artifact (serve it with any static web server) |

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

## Milestone 3: Native Studio foundation (**Planned**)

Dear ImGui (docking) on SDL3: project open/create, hierarchy (adds parent/
child to the scene model), viewport with selection and move tools, component
inspector driven by the same component descriptors as the file format, asset
browser, save/load, play/stop (the `Scene` copy already supports this),
console (a `px::log` sink).

## Milestone 4: Multiplayer prototype (**Planned**)

Dedicated server built from `px-headless`; client/server transport (to be
evaluated: GameNetworkingSockets, ENet, or WebTransport/WebSocket for browser
compatibility); session lifecycle; player spawning; server-authoritative
`GameSession` consuming client `InputFrame`s; snapshot replication; disconnect
handling; local multi-client test tool.

## Milestone 5: Visual scripting and expanded engine (**Planned**)

Typed node graphs compiled to the same gameplay API as Luau; graph editor;
tilemap and animation editors; animation clip assets; tileset assets with
per-tile properties (one-way platforms, slopes).

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

1. **Next: Milestone 3 (Studio foundation), with prefabs and multiple
   behaviours per entity in scope.** Scripting now works, so the bottleneck
   for creators is editing scenes by hand in JSON. Studio's inspector should
   be driven by the same component and script-property descriptors that the
   file format and scripting use, which also prepares visual scripting.
2. **Keep the WebAssembly job in CI, but defer the browser runtime itself**
   (ADR 0012): it already builds and runs, so regressions are caught for
   free; productising it (loading projects over the network, asset
   streaming, page integration, size budgets) belongs with the platform
   work in Milestones 6-7.
3. **Add gamepad support early** (small, and platformers need it to feel right).
4. **Keep the simulation deterministic** (fixed ticks, input frames, no
   wall-clock reads in gameplay). It is already tested and makes replays,
   automated playtests and server reconciliation much easier.
