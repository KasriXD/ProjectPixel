# Roadmap

Status key: **Done** (implemented and verified), **Partial**, **Planned**.
"Verified" always names where it was verified.

## Current focus

Milestones 0-7 are done. Milestone 7 made the clients cross-platform:
browsers play platform games on the same game servers as desktop players
(WebSockets, `wss://`), the platform and its clients speak HTTPS, every game
works with gamepads and touch screens, the player has settings, and Android
and iOS apps are built in CI. CI builds and tests on Linux, Windows (MSVC),
macOS, WebAssembly, Android and iOS, and plays a browser and a desktop
player together. Recommended next: people play it on real devices (a phone,
a gamepad, a browser on another computer), then a player launcher (see
"Recommended adjustments" below).

## Blockers and open items

| Item | Impact | Resolution |
|---|---|---|
| Not yet run on real macOS hardware (CI passes) | Rendering/input on Macs unconfirmed | A Mac check when one is available. Windows: played on a Windows 11 PC (2026-10-23) |
| No real gamepad, touch screen or phone tried yet (Milestone 7 checks are automated) | Feel of the controls, layouts on real screens, notches, performance on phones unconfirmed | Founder: a gamepad on the PC; the CI-built APK on an Android phone; `/play` on a phone's browser |
| Browser play not yet tried by a person | Input feel, audio policies, page integration unconfirmed | Founder: start px-platform with `--web-player` (the CI artifact `px-player-web`) and open `/play` |
| Multiplayer played only on one computer (separate processes over real UDP) and in simulated networks so far | Behaviour over Wi-Fi and the internet (latency spikes, NAT, firewalls) unconfirmed | Founder runs Studio's *Test Multiplayer*, then `px-server` on one PC and `px-player --connect` from another on the same network |
| Game servers run as plain processes; UDP game traffic is not encrypted | Not ready for the internet | HTTPS is in place; containers and encrypted UDP are the next platform items |
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
- [x] Prefabs: done in Milestone 5 together with spawning entities from
      scripts

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
- [x] Connect tokens from the platform (Milestone 6); encryption and per-client rate limits still planned
- [ ] Interest management for large worlds; lag compensation for shooting
- [ ] A browser transport (WebSocket or WebTransport) for web clients

## Milestone 5: Visual scripting and expanded engine (**Done**)

Verified on Linux (GCC, Clang), in CI on Linux (GCC, Clang, AddressSanitizer
+ UBSan, headless), Windows (MSVC), macOS and WebAssembly, and with the
MinGW-w64 build under Wine 9 (unit tests, playthrough, network test); each
tool was used in Studio on Linux.

- [x] Prefabs ([docs/prefabs.md](docs/prefabs.md), ADR 0019): saved entity
      trees; copies in scenes with derived ids; *Apply To Prefab* updates every
      copy in every scene by a three-way merge, so each copy keeps its own
      changes; revert; copies survive copy/paste, duplicate and undo
- [x] Spawning: `World.spawn(prefab, position?, parent?)`, deterministic ids,
      per-tick limits; spawned entities replicate to multiplayer clients
- [x] Animation clips ([docs/animation.md](docs/animation.md)): clip assets
      from sprite sheets, an `Animator` that picks idle/run/jump/fall from
      the controller, `entity:playAnimation` from scripts, an animation editor
      with a frame picker and preview
- [x] Tilesets ([docs/tilesets.md](docs/tilesets.md)): per-tile collision
      (solid, none, one-way, two slopes) and tags, `World.tileAt` /
      `World.tileHasTag`; one-way `BoxCollider`s; ground snapping on slopes;
      tileset editor; rectangle and bucket-fill tile tools
- [x] Visual scripting ([docs/visual-scripting.md](docs/visual-scripting.md),
      ADR 0020): graphs compiled to Luau, so they share the VM, sandbox,
      limits and realms; the node catalog is generated from the API
      reference; problems and runtime errors name the node; graph editor in
      Studio with typed pins, filtered add menu, undo and a Luau view
- [x] Examples use all of it: prefab coins and gems spawning a sparkle,
      animated heroes, slopes and one-way planks in the platformer, water
      tagged in the top-down tileset, clouds drifting by a visual script

Next for these tools (not part of the milestone):

- [ ] Graph functions/sub-graphs, comments and groups, execution
      highlighting while playing; `require` for Luau modules
- [ ] Nested prefab editing (open a prefab on its own canvas) and a view of a
      copy's own changes
- [ ] Animation events (a frame calls a script), blend of clips for
      top-down directions
- [ ] Auto-tiling rules in tilesets

## Milestone 6: Platform backend and publishing (**Done**: the core loop)

Scope agreed with the founder: the core loop end to end; profiles, friends
and fuller moderation tools later. Verified in CI on Linux (GCC, Clang,
AddressSanitizer + UBSan, headless), Windows (MSVC) and macOS, and with the
MinGW-w64 build under Wine 9, where an end-to-end test publishes the
examples, starts real game servers through the platform, plays on them over
UDP and checks the saved data; used on Linux with px-platform, Studio's
Platform panel, px-player and px-headless bots.

- [x] `px-platform` ([docs/platform.md](docs/platform.md), ADR 0021): C++
      service in this repository, HTTP/JSON API, SQLite, rate limits
- [x] Accounts: Argon2id passwords, 30-day session tokens stored as hashes,
      a login shared by Studio and the player
- [x] Game packages (`*.pxpkg`) and publishing with versions: every upload
      is checked in a separate, limited process (everything loads, images
      re-encoded, two seconds simulated); content-addressed storage
- [x] Catalog: public/unlisted/private games, search, popular, playing now,
      recently updated; a web page listing the games
- [x] Game servers on demand (ADR 0022): the platform starts px-server
      processes, fills seats, stops empty servers; signed, single-use,
      60-second connect tokens checked by the servers; one game at a time
      per player
- [x] Saved player data (ADR 0023): `Data.get/set/increment`, loaded before
      joining, saved while playing and on leaving, limits; kept between plays
      in Studio
- [x] Moderation: reports, hide games (servers stop), ban accounts, audit log
- [x] Clients: Studio's Platform panel (log in, publish, your games, play,
      report, moderation), `px-player --platform --play`, `px-headless`
      `--publish` / `--play` / `--pack` / `--check-package`

Next for the platform (not part of the milestone):

- [ ] HTTPS in the clients and encrypted game traffic (keys in the token)
- [ ] A player launcher (log in, browse, play) without Studio; thumbnails
- [ ] Game servers in containers on separate hosts; PostgreSQL; several
      service instances
- [ ] Profiles and avatars, friends and invitations, playing together on
      purpose (parties, server choice)
- [ ] Email verification, password reset, two-factor authentication,
      account deletion; text filtering
- [ ] Data shared between players (leaderboards), data of offline players

## Milestone 7: Cross-platform clients (**Done**: browser cross-play, HTTPS, input devices, mobile builds)

Scope agreed with the founder: browser cross-play first, with HTTPS; Android
and iOS apps built in CI, not tested on devices. Verified in CI on Linux
(GCC, Clang, AddressSanitizer + UBSan, headless), Windows (MSVC), macOS,
WebAssembly, Android and iOS, and with the MinGW-w64 build under Wine 9 (all
186 unit tests, including HTTPS, WebSockets and the browser page).

- [x] HTTPS ([ADR 0025](docs/adr/0025-tls-with-mbed-tls.md)): Mbed TLS in
      the platform (`--https-port`, `--dev-certificate`) and every client,
      with the system's certificate stores; tested with a development
      certificate
- [x] WebSockets for browsers ([ADR 0024](docs/adr/0024-websockets-for-browsers.md)):
      game servers listen for UDP and WebSockets at once (`wss://` with
      TLS); desktop players can use either
- [x] Browser play on the platform: the `/play` page (log in, catalog,
      play) and the WebAssembly player loading the game package; strict
      content security policy. CI: headless Chromium plays the top-down
      route on a platform server while a desktop bot is there over UDP,
      over HTTP and HTTPS
- [x] Gamepads ([input-devices.md](docs/input-devices.md)): buttons stand for
      the usual keys, so every game works with a pad; the stick in 8 sectors
- [x] Touch controls generated from a game's actions (direction pad, up to
      four buttons, pause); shown on touch; `--touch` to try with a mouse
- [x] Player settings menu (Esc, Start, pause button): VSync, frame limit,
      scaling, FPS, touch controls; saved per device; pauses when alone
- [x] Android app ([ADR 0026](docs/adr/0026-mobile-apps.md)): Gradle + NDK,
      the player as `libmain.so`, the game in the APK's assets; CI builds a
      release APK (arm64-v8a, x86_64) and starts it on an emulated phone
- [x] iOS app: an app bundle from Xcode with the game inside; CI runs it on a
      simulated iPhone (UIKit, Metal)

Next for the clients (not part of the milestone):

- [ ] A launcher in the desktop and mobile apps (log in, browse, play), the
      platform client enabled on phones
- [ ] Testing on real phones, gamepads and browsers by people; safe areas
- [ ] Store builds (signing, icons, review rules); compressed web downloads
- [ ] WebTransport for browsers on lossy networks; encrypted UDP

## Milestone 8: Creator ecosystem and scaling (**Planned**)

Persistent worlds, social hubs, collaboration, marketplace, analytics,
monetization, advanced moderation, scalable hosting.

---

## Recommended adjustments

1. **Next: people on real devices, then a launcher.** Run px-platform on
   one computer of a home network (`--bind 0.0.0.0 --public-host <address>
   --web-player ...`), let a few people play from PCs, phones' browsers and
   the Android app, with gamepads, and fix what they hit. For people who
   only play, a launcher in the apps matters more than Studio's panel.
   Before anything faces the internet: game servers in containers and
   encrypted UDP traffic.
2. **Client-side behaviours before more visual effects**: decorative motion
   (the drifting clouds) currently runs on the server and is replicated
   (about 60 bytes per snapshot for 8 clouds). Client-realm behaviours
   (ADR 0011) would run it locally for free.
3. **Keep the browser and mobile builds in CI**: they catch regressions on
   every change; download size (compression, `-Oz`) is the next browser
   item.
4. **Keep the simulation deterministic** (fixed ticks, input frames, no
   wall-clock reads in gameplay). It is already tested and makes replays,
   automated playtests and server reconciliation much easier.
