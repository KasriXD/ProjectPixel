# Platform support

SDL3 runs on many platforms. That does **not** mean every engine feature works
on all of them: each platform needs its own build, packaging, input and
testing. This page states what is verified and what work remains.

## Status

| Platform | Builds | Runs | Verified how |
|---|---|---|---|
| Linux x64 (X11) | Yes: GCC 13, Clang 18 | Yes: SDL `opengl` renderer on Mesa | Built and tested locally (Ubuntu 24.04), screenshots inspected |
| No display (any OS) | Yes | Yes: SDL `dummy` driver + `software` renderer | CTest `player_smoke_headless_display` |
| Linux (Wayland) | Compiled in | **Not tested** | |
| Headless (no SDL) | Yes | Yes | `headless` preset, `px-headless` playthrough test |
| Windows x64 (MinGW-w64 GCC 13) | Yes, `-O3 -Werror` | Under Wine 9: all unit tests, scripted playthrough, and the client (SDL `windows` driver, `direct3d` renderer) | Cross-compiled on Linux; screenshots inspected. Not yet run on real Windows |
| Windows x64 (MSVC 2022) | Yes | Unit tests, playthrough and windowless client test pass | GitHub Actions `windows-2022`. Not yet run interactively on real hardware |
| macOS arm64 (Apple Clang) | Yes | Unit tests, playthrough and windowless client test pass | GitHub Actions `macos-14`. Not yet run in a real window; needs macOS 13.3+ |
| Android, iOS | No | No | Planned, Milestone 7 |
| WebAssembly under Node.js (engine, Luau, tests) | Yes: Emscripten 6.0.13 | All unit tests and the scripted playthrough pass | Experimental CI job |
| Browser (WebAssembly + WebGL) | Yes: `px-player.html` | See "Browser run" below | Experimental CI job, headless Chromium |

## Desktop

Expected to work on Windows and macOS with the same code: SDL3 picks
Direct3D 11/12 or Metal for `SDL_Renderer`. Known requirements:

- Windows: Visual Studio 2022 17.8+ (C++20 `<format>`), CMake ≥ 3.24.
- macOS: Xcode 15.3+, deployment target 13.3 (Apple's libc++ gates
  floating-point `std::to_chars`/`std::format` on it).

## Mobile (planned)

Work required:

- Android: Gradle project wrapping the CMake build (SDL ships an Android
  project template), APK asset access through an `IFileSystem` implementation
  using `SDL_IOStream` (project files live inside the APK, not on a normal
  filesystem), lifecycle handling (pause/resume, losing the GPU context).
- iOS: Xcode project, code signing, app bundle file access, no JIT (Luau
  native code generation must be disabled), App Store review rules for
  user-generated content.
- Both: touch controls mapped to actions, safe areas and notches, aspect
  ratios (virtual resolution + letterbox), performance budgets, background
  audio rules.

## Browser

**Status: experimental, verified in CI; not a supported product yet.**
Decision and reasoning: [ADR 0012](adr/0012-browser-runtime.md).

### What was verified

- Luau compiles to WebAssembly unmodified. Its error handling (C++
  exceptions), CPU interrupt, memory cap and read-only sandbox were checked
  in WebAssembly under Node.js.
- With Emscripten 6.0.13, the whole engine compiles; all unit tests and the
  scripted playthrough pass under Node.js, and the playthrough ends at the
  same player position as the native Linux and Windows builds.
- SDL3 + `px-player` build as `px-player.html` with the example project
  preloaded: `px-player.wasm` 2.5 MB, `.js` 177 KB, `.data` 30 KB
  (uncompressed).
- Emscripten 3.1.6 (Ubuntu 24.04's package) is **too old**: its C++ library
  lacks `std::format`.

### Browser run

Result pending: filled in from the CI run of the headless Chromium step.

### Building it yourself

```sh
# Emscripten SDK 6.0.13 (https://emscripten.org/docs/getting_started/downloads.html)
source /path/to/emsdk/emsdk_env.sh
cmake --preset wasm && cmake --build --preset wasm
cd build/wasm/apps/player && python3 -m http.server 8000
# open http://localhost:8000/px-player.html
```

`cmake --preset wasm-headless` builds the engine and tests for Node.js
(`ctest --preset wasm-headless`). URL parameters become command-line
arguments: `px-player.html?arg=--debug`.

### Remaining work for a real browser runtime

- Loading projects from the platform (an `IFileSystem` over downloaded,
  validated packages) instead of files baked into `.data`.
- Download size: compression, `-Oz`, excluding unused SDL subsystems.
- Graphics: SDL_Renderer's WebGL path today; WebGL2/WebGPU when a custom
  GPU renderer exists.
- Networking via WebSockets or WebTransport only (no raw UDP), which the
  Milestone 4 transport choice must respect.
- Threads only with cross-origin isolation (`SharedArrayBuffer`); the
  engine is single-threaded today.
- Page integration: loading screen, fullscreen, audio unlock on first input.
