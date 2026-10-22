# ADR 0012: Browser runtime: feasibility and timing

- Status: Accepted
- Date: 2026-10-23

## Context
The vision includes browser play for compatible, less demanding games. The
roadmap recommended checking browser feasibility early (Milestone 2) so
that desktop-only assumptions would not creep into the engine. The question
was whether the current stack (SDL3 3.4.18, Luau 0.743, CMake, our module
layout) can support a WebAssembly proof of concept, what it costs, and
whether to build the browser runtime now.

## Findings (measured, not assumed)

| Check | Result | Where |
|---|---|---|
| Luau compiles to WebAssembly | Yes, unmodified, with Emscripten 3.1.6 and 6.0.13 | local + CI |
| Luau sandbox mechanics in wasm: errors (C++ exceptions), CPU interrupt, memory cap and recovery, read-only tables | All work | local probe under Node.js |
| Whole engine compiles (C++20, `std::format`) | Yes with Emscripten 6.0.13; **no** with 3.1.6 (its libc++ lacks `std::format`) | CI / local |
| All 75 unit tests under Node.js | Pass | CI |
| Scripted playthrough under Node.js | Level completed; final player position identical to native Linux and Windows builds | CI |
| SDL3 + px-player browser build | Builds: `px-player.wasm` 2.5 MB, `.js` 177 KB, `.data` 30 KB (uncompressed) | CI |
| Browser build runs in headless Chromium (Playwright) | Yes: WebGL 2.0, SDL `emscripten` driver + `opengles2` renderer; scripted level completed, 14/14 coins, 0 script errors, same simulated completion time as native | CI |

Code changes that were needed: enabling C++ exceptions for Emscripten
(Luau and the project parser rely on them), memory growth, a 1 MiB stack,
Node file-system access for the headless tools, and preloading the example
project for the browser player. No engine code had to change: the SDL main
callbacks, `IFileSystem` and the absence of threads, JIT and blocking loops
paid off.

## Decision
- **Keep the WebAssembly build in CI** as an experimental, non-blocking job:
  build engine, Luau and SDL3; run all tests and the playthrough under Node.js;
  run the browser player in headless Chromium. This keeps the engine
  browser-compatible at almost no cost.
- **Defer the browser runtime as a product** to the platform milestones
  (6-7). What remains is mostly platform work, not engine work:
  - loading projects over the network instead of preloaded files
    (an `IFileSystem` for downloaded, validated packages);
  - size budgets (2.5 MB wasm before compression; strip unused SDL
    subsystems, enable `-Oz`, Brotli on the server) and a "browser
    compatible" flag for games;
  - page integration: loading screen, focus and fullscreen, audio unlock
    on first input (audio is not implemented yet anyway);
  - WebGL vs WebGPU once a custom GPU renderer exists (ADR 0003);
  - WebSocket/WebTransport networking for multiplayer (Milestone 4 must
    choose a transport that has a browser path);
  - threads only with cross-origin isolation, if ever needed.

Estimated effort for a usable public browser player once the platform can
serve game packages: 2-4 weeks of focused work, dominated by asset loading,
size optimisation and page integration, not by engine porting.

## Consequences
- Engine changes now get a browser compile and test for free; a desktop-only
  dependency or API (threads, blocking I/O, native-only libraries) shows up as
  a failing wasm job.
- Emscripten must stay at a version whose libc++ has `std::format`
  (pinned to 6.0.13 in CI).
- Luau native code generation stays off everywhere; script performance in the
  browser is interpreter performance (adequate for the scale targeted:
  1,000 scripted Tick handlers cost about 1 ms per tick natively).
