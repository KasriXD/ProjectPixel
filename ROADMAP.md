# Roadmap

Status key: **Done** (implemented and verified), **Partial**, **Planned**.
"Verified" always names where it was verified.

## Current focus

Milestones 0 and 1 are done on Linux. Next: verify Windows and macOS through
CI, then Milestone 2 (Luau scripting).

## Blockers and open items

| Item | Impact | Resolution |
|---|---|---|
| MSVC and macOS builds not yet compiled; Windows only tested via MinGW + Wine | Can't claim full desktop support | Push to GitHub: `.github/workflows/ci.yml` builds and tests on Windows (MSVC), macOS and Linux. Founder to try the Windows package on a real PC |
| Gamepad input not implemented | Keyboard only | Small change in `platform_sdl` + bindings; scheduled with Milestone 7, could move earlier |

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
- [x] CI workflow for Linux, Windows, macOS (written; **runs on first push**)

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

## Milestone 2: Gameplay and scripting (**Planned**, next)

Plan in [docs/scripting.md](docs/scripting.md). First slice:

- [ ] Fetch Luau 0.743 (MIT) as a pinned dependency; build `Luau.VM` + `Luau.Compiler` only
- [ ] `px::scripting` module: one sandboxed VM per session, `luaL_sandbox`, allocator-based memory cap, `interrupt`-based instruction budget
- [ ] `Script` component referencing a script asset; lifecycle `onStart`, `onTick(dt)`, `onTriggerEnter(other)`, `onDestroy`
- [ ] Gameplay API v1 (`px.entity`, transforms, components, events, input actions, logging)
- [ ] Error reporting with file/line, shown in the log and on screen
- [ ] Example: coin behaviour and a moving platform written in Luau
- [ ] API reference generated from the binding definitions

Deliverable: game behaviour customized by Luau scripts without recompiling.

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

1. **Validate the browser build during Milestone 2, not Milestone 7.**
   An Emscripten build of `px-player` (SDL3 + `SDL_Renderer` → WebGL) is cheap
   now and catches desktop-only assumptions (blocking loops, filesystem
   access, threads, Luau JIT) before more code depends on them. Luau has a
   web build option, so scripting can be validated in the same step.
   The engine already avoids the two biggest traps: the main loop uses SDL
   callbacks, and file access goes through `IFileSystem`.
2. **Add gamepad support early** (small, and platformers need it to feel right).
3. **Keep the simulation deterministic** (fixed ticks, input frames, no
   wall-clock reads in gameplay). It is already tested and makes replays,
   automated playtests and server reconciliation much easier.
