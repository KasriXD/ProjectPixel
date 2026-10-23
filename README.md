# Project Pixel

> **Project Pixel** is a working title. In code, files and executables the
> project uses the neutral prefix `px`; the user-facing name is a single CMake
> variable (`PX_PRODUCT_NAME`). See [ADR 0001](docs/adr/0001-language-build-and-naming.md).

A 2D user-generated game platform: an engine, a creator studio and an online
platform where anyone can make, publish and play 2D games. The long-term
vision is in [PROJECT_VISION.md](PROJECT_VISION.md).

![Studio editing the example platformer](docs/images/studio.png)

## Current status (version 0.3.0)

Milestones 0 (engineering foundation), 1 (engine prototype), 2 (Luau
scripting) and 3 (Studio foundation) are complete. See
[ROADMAP.md](ROADMAP.md) for detail.

| Area | Status |
|---|---|
| CMake/C++20 build, pinned dependencies, warnings-as-errors | Implemented, verified on Linux (GCC 13, Clang 18) |
| Window, main loop, keyboard input (SDL3) | Implemented, verified on Linux (X11 + OpenGL) and Windows (Direct3D, under Wine); windowless smoke test passes in CI on Linux, Windows and macOS |
| 2D rendering: sprites, tilemaps, layers, camera, pixel-perfect scaling, debug text | Implemented, verified by screenshots |
| Entity/component scene model with stable ids, parent/child hierarchy | Implemented, unit tested |
| Studio (desktop editor): projects, hierarchy, scene view, inspector, assets, script editor, undo, play/stop, console | Implemented; a game was built in it from an empty project and runs in `px-player`. Verified on Linux (X11) and under Wine (Direct3D); builds in CI on Windows (MSVC) and macOS. **Not yet used on a real Windows or macOS desktop** ([guide](docs/studio.md)) |
| Kinematic collision: solid tiles/boxes, triggers | Implemented, unit tested |
| Platformer movement (coyote time, jump buffer, variable jump) | Implemented, unit tested |
| Versioned project/scene file format with validation | Implemented, unit tested |
| Headless simulation (no graphics) | Implemented; basis of the future dedicated server |
| Luau scripting: sandbox, CPU/memory limits, lifecycle, gameplay API, several behaviours per entity | Implemented; verified on Linux, Windows (MSVC CI, MinGW + Wine), macOS (CI), WebAssembly (CI) |
| Example platformer, loaded from project files | Playable; coins, win condition and a moving platform are Luau scripts; an automated run completes the level |
| Hot reload of scripts and scenes (F5 in px-player) | Implemented, verified manually with simulated key presses |
| Windows | Builds and passes all tests in CI with Visual Studio 2022; MinGW-w64 build runs the game under Wine 9 (Direct3D renderer). **Not yet played on a real Windows PC** |
| macOS | Builds and passes all tests in CI (Apple Clang, arm64). **Not yet played in a real window** |
| Memory safety | Unit tests, headless playthrough, player and Studio smoke tests run clean under AddressSanitizer + UBSan with leak detection |
| Browser (WebAssembly) | **Experimental**: engine and tests pass under Node.js in CI; the browser build (WebGL 2) completes the scripted level in headless Chromium in CI. Not yet played by a person ([details](docs/platform-support.md#browser)) |
| Prefabs, gamepad, touch, audio, text/fonts in games, visual scripting, multiplayer, platform services | Planned (Milestones 4-8) |

## Building

### Prerequisites

- CMake 3.24+ and a C++20 compiler: GCC 13+, Clang 16+, or Visual Studio 2022 17.8+
  (Apple Clang from Xcode 15.3+, macOS 13.3+)
- Ninja (recommended; Visual Studio generator also works on Windows)
- Git, and internet access on the first configure (SDL3 and nlohmann/json are
  downloaded and verified by SHA-256; see [docs/dependencies.md](docs/dependencies.md))
- Python 3 with Pillow, only to regenerate the example art (`tools/`)

**Linux** also needs SDL3's system development headers. On Ubuntu/Debian:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
  libx11-dev libxext-dev libxcursor-dev libxi-dev libxrandr-dev libxfixes-dev \
  libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev wayland-protocols \
  libegl1-mesa-dev libgl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev \
  libasound2-dev libpulse-dev libdbus-1-dev libudev-dev
```

### Build and test

```sh
cmake --preset dev            # configure (Debug, Ninja) into build/dev
cmake --build --preset dev    # first build also compiles SDL3 (~1-3 minutes)
ctest --preset dev            # unit tests, headless playthrough, player smoke test
```

`ctest` also runs Studio once with SDL's dummy video driver
(`studio_smoke_headless_display`, about 15 seconds).

Other presets: `release`, `asan` (AddressSanitizer + UBSan), `headless` (no
SDL or graphics at all), and `vs2022` on Windows
(`cmake --preset vs2022` then `cmake --build --preset vs2022-debug`).

Windows executables can also be cross-compiled from Linux with MinGW-w64:

```sh
sudo apt install g++-mingw-w64-x86-64-posix
cmake -S . -B build/mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake
cmake --build build/mingw
```

The resulting `px-player.exe` and `px-studio.exe` need no extra DLLs. Copy
`examples/` next to them: `px-player.exe` then starts the example with no
arguments, and Studio uses it as the template for new projects.

### Make a game in Studio

```sh
./build/dev/apps/studio/px-studio                      # welcome screen: new or open project
./build/dev/apps/studio/px-studio path/to/project      # open a project directly
```

Create a project (empty, or from the platformer template), paint a level,
add a player and coins, attach Luau behaviours, press F5 to play and Ctrl+S
to save. The [Studio guide](docs/studio.md) has a ten-minute tutorial that
builds a game from an empty project, plus every panel, shortcut and
limitation.

### Run

```sh
./build/dev/apps/player/px-player                      # plays examples/platformer
./build/dev/apps/player/px-player path/to/project      # any project folder
./build/dev/apps/headless/px-headless examples/platformer --ticks 600
```

Controls: arrows or A/D to move, Space/Z/W/Up to jump, R restart, F5 reload
scripts and scenes from disk, F1 debug overlay (colliders, FPS, player
position, script memory), F2 screenshot, Esc quit.

Useful options: `--frames N --screenshot out.png` runs a fixed number of
deterministic frames and saves the last one; `--input-script FILE` plays
recorded input (see [docs/input-scripts.md](docs/input-scripts.md)). Run with
`--help` for the full list.

### Try scripting

The example's coins, moving platform and win condition are Luau scripts in
`examples/platformer/scripts/`. With `px-player` running:

1. Open `examples/platformer/scripts/coin.luau` and change
   `Game.addScore(self.value)` to `Game.addScore(self.value * 10)`, or change
   `period = 4` in `moving_platform.luau` to `period = 2`.
2. Press **F5**. The project reloads from disk; no recompiling.
3. Introduce a mistake (e.g. misspell `self.value`) and press F5 again: the
   error, with file and line, appears on screen while the game keeps running.

Headless checks: `px-headless examples/platformer --input-script
examples/platformer/tests/full_run.input --log-events --expect-level-complete`.
Guide: [docs/scripting.md](docs/scripting.md); every API function:
[docs/scripting-api.md](docs/scripting-api.md).

## Repository layout

```
engine/            engine modules (static libraries), one folder per module
  core/            logging, errors, math, ids, sandboxed file access, timing, key names
  assets/          asset ids, asset registry, image decoding
  scene/           entities, components, scenes
  physics/         kinematic AABB collision
  project/         project and scene file formats
  scripting/       Luau sandbox, resource limits, gameplay API, Script lifecycle
  runtime/         headless fixed-tick game simulation and gameplay systems
  render/          renderer interface, camera, scene-to-draw-call extraction
  platform_sdl/    SDL3 window, input and renderer backend
  editor/          editor model without UI: projects, scene documents, undo, play mode
apps/player/       px-player, the desktop client
apps/headless/     px-headless, graphics-free runner (seed of the dedicated server)
apps/studio/       px-studio, the desktop editor (Dear ImGui)
examples/          example game projects (content only, no code)
tests/             automated tests
tools/             developer tools (example generator)
docs/              detailed documentation and architecture decision records
third_party/       vendored libraries (stb_image, doctest, Luau subset, Dear ImGui)
```

## Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md): modules, boundaries and data flow
- [ROADMAP.md](ROADMAP.md): milestones, progress, next steps
- [CONTRIBUTING.md](CONTRIBUTING.md): coding standards and workflow
- [docs/studio.md](docs/studio.md): Studio guide and tutorial
- [docs/project-format.md](docs/project-format.md): `project.json` and scene files
- [docs/components.md](docs/components.md): generated reference of every component and field
- [docs/engine.md](docs/engine.md): coordinates, scene model, simulation loop, rendering
- [docs/platform-support.md](docs/platform-support.md): desktop, mobile and browser: what is verified and what is planned
- [docs/scripting.md](docs/scripting.md): Luau scripting guide: lifecycle, sandbox, limits, realms, versioning
- [docs/scripting-api.md](docs/scripting-api.md): generated API reference
- [docs/visual-scripting.md](docs/visual-scripting.md): visual scripting design (planned)
- [docs/security.md](docs/security.md): threat model and current protections
- [docs/adr/](docs/adr/): architecture decision records
