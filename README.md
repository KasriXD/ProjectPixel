# Project Pixel

> **Project Pixel** is a working title. In code, files and executables the
> project uses the neutral prefix `px`; the user-facing name is a single CMake
> variable (`PX_PRODUCT_NAME`). See [ADR 0001](docs/adr/0001-language-build-and-naming.md).

A 2D user-generated game platform: an engine, a creator studio and an online
platform where anyone can make, publish and play 2D games. The long-term
vision is in [PROJECT_VISION.md](PROJECT_VISION.md).

![Example platformer running in px-player](docs/images/example-platformer.png)

## Current status (version 0.1.0)

Milestone 0 (engineering foundation) is complete and Milestone 1 (engine
prototype) is complete on Linux. See [ROADMAP.md](ROADMAP.md) for detail.

| Area | Status |
|---|---|
| CMake/C++20 build, pinned dependencies, warnings-as-errors | Implemented, verified on Linux (GCC 13, Clang 18) |
| Window, main loop, keyboard input (SDL3) | Implemented, verified on Linux (X11 + OpenGL) and Windows (Direct3D, under Wine); windowless smoke test passes in CI on Linux, Windows and macOS |
| 2D rendering: sprites, tilemaps, layers, camera, pixel-perfect scaling, debug text | Implemented, verified by screenshots |
| Entity/component scene model with stable ids | Implemented, unit tested |
| Kinematic collision: solid tiles/boxes, triggers | Implemented, unit tested |
| Platformer movement (coyote time, jump buffer, variable jump) | Implemented, unit tested |
| Versioned project/scene file format with validation | Implemented, unit tested |
| Headless simulation (no graphics) | Implemented; basis of the future dedicated server |
| Example platformer, loaded from project files | Playable; scripted run collects all coins in an automated test |
| Windows | Builds and passes all tests in CI with Visual Studio 2022; MinGW-w64 build runs the game under Wine 9 (Direct3D renderer). **Not yet played on a real Windows PC** |
| macOS | Builds and passes all tests in CI (Apple Clang, arm64). **Not yet played in a real window** |
| Memory safety | Unit tests, headless playthrough and player run clean under AddressSanitizer + UBSan with leak detection |
| Gamepad, touch, audio, text/fonts, Luau scripting, Studio, multiplayer, platform services | Planned (Milestones 2-8) |

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

The resulting `px-player.exe` needs no extra DLLs. Copy `examples/` next to it
and it starts the example with no arguments.

### Run

```sh
./build/dev/apps/player/px-player                      # plays examples/platformer
./build/dev/apps/player/px-player path/to/project      # any project folder
./build/dev/apps/headless/px-headless examples/platformer --ticks 600
```

Controls: arrows or A/D to move, Space/Z/W/Up to jump, R restart, F1 debug
overlay (colliders, FPS, player position), F2 screenshot, Esc quit.

Useful options: `--frames N --screenshot out.png` runs a fixed number of
deterministic frames and saves the last one; `--input-script FILE` plays
recorded input (see [docs/input-scripts.md](docs/input-scripts.md)). Run with
`--help` for the full list.

## Repository layout

```
engine/            engine modules (static libraries), one folder per module
  core/            logging, errors, math, ids, sandboxed file access, timing, key names
  assets/          asset ids, asset registry, image decoding
  scene/           entities, components, scenes
  physics/         kinematic AABB collision
  project/         project and scene file formats
  runtime/         headless fixed-tick game simulation and gameplay systems
  render/          renderer interface, camera, scene-to-draw-call extraction
  platform_sdl/    SDL3 window, input and renderer backend
apps/player/       px-player, the desktop client
apps/headless/     px-headless, graphics-free runner (seed of the dedicated server)
examples/          example game projects (content only, no code)
tests/             automated tests
tools/             developer tools (example generator)
docs/              detailed documentation and architecture decision records
third_party/       vendored single-header libraries
```

## Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md): modules, boundaries and data flow
- [ROADMAP.md](ROADMAP.md): milestones, progress, next steps
- [CONTRIBUTING.md](CONTRIBUTING.md): coding standards and workflow
- [docs/project-format.md](docs/project-format.md): `project.json` and scene files
- [docs/engine.md](docs/engine.md): coordinates, scene model, simulation loop, rendering
- [docs/platform-support.md](docs/platform-support.md): desktop, mobile and browser: what is verified and what is planned
- [docs/scripting.md](docs/scripting.md): Luau scripting plan (Milestone 2)
- [docs/security.md](docs/security.md): threat model and current protections
- [docs/adr/](docs/adr/): architecture decision records
