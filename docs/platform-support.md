# Platform support

SDL3 runs on many platforms. That does **not** mean every engine feature works
on all of them: each platform needs its own build, packaging, input and
testing. This page states what is verified and what work remains.

## Status

| Platform | Builds | Runs | Verified how |
|---|---|---|---|
| Linux x64 (X11) | Yes: GCC 13, Clang 18 | Yes: SDL `opengl` renderer on Mesa | Built and tested locally (Ubuntu 24.04), screenshots inspected |
| Linux, no display | Yes | Yes: SDL `offscreen` driver + `software` renderer | CTest `player_smoke_offscreen` |
| Linux (Wayland) | Compiled in | **Not tested** | |
| Headless (no SDL) | Yes | Yes | `headless` preset, `px-headless` playthrough test |
| Windows x64 (MinGW-w64 GCC 13) | Yes, `-O3 -Werror` | Under Wine 9: all unit tests, scripted playthrough, and the client (SDL `windows` driver, `direct3d` renderer) | Cross-compiled on Linux; screenshots inspected. Not yet run on real Windows |
| Windows x64 (MSVC) | **Not yet verified** | **Not yet verified** | CI job written (`windows-2022`) |
| macOS (arm64) | **Not yet verified** | **Not yet verified** | CI job written (`macos-14`); needs macOS 13.3+ |
| Android, iOS | No | No | Planned, Milestone 7 |
| Browser | No | No | Recommended for Milestone 2 (see ROADMAP) |

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

## Browser (planned)

A native binary cannot run in a browser. A browser runtime requires:

- Compiling the engine with **Emscripten** to WebAssembly.
- A browser graphics backend: SDL3's `SDL_Renderer` has a WebGL path; a later
  GPU backend would need WebGL2 or WebGPU.
- No blocking main loop: already handled (SDL main callbacks).
- File access through an `IFileSystem` backed by downloaded packages
  (already abstracted), not the local disk.
- Threads only with cross-origin isolation (`SharedArrayBuffer`); design for
  single-threaded first.
- Networking via WebSockets or WebTransport only (no raw UDP), which affects
  the Milestone 4 transport choice.
- Download size budgets: the browser runtime should load "compatible, less
  demanding" games; the platform should flag which games qualify.
