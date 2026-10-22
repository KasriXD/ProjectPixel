# Dependencies

Every dependency was evaluated for purpose, license, maintenance, platform
support and build impact before being added (policy: [CONTRIBUTING.md](../CONTRIBUTING.md#adding-a-dependency),
mechanism: [ADR 0002](adr/0002-dependency-management.md)).

## In use

| Dependency | Version | License | How | Used by |
|---|---|---|---|---|
| SDL3 | 3.4.18 | zlib | Downloaded release tarball, SHA-256 pinned, built as a static library; a system SDL3 ≥ 3.4 is used instead if found | `platform_sdl` (client only) |
| nlohmann/json | 3.12.0 | MIT | Downloaded release tarball, SHA-256 pinned (header-only); system package ≥ 3.11 used if found | `project` (private) |
| stb_image | 2.30 | MIT / public domain | Vendored in `third_party/stb`, compiled in one C file with only PNG/JPEG/BMP/TGA enabled and no stdio | `assets` (private) |
| doctest | 2.5.3 | MIT | Vendored in `third_party/doctest` | tests only |

### SDL3
- **Why:** windowing, input, controllers, audio and a portable 2D renderer on
  Windows, macOS, Linux, Android, iOS and Emscripten. The industry standard
  for this layer; actively maintained.
- **Notes:** SDL running on a platform does not mean every engine feature works
  there; see [platform-support.md](platform-support.md). Linux builds need
  X11/Wayland development headers (listed in the README).
- **Build impact:** ~1 minute on 2 cores, once per build directory.

### nlohmann/json
- **Why:** robust, well-known JSON parser with good error positions. Kept
  private to `project`; the rest of the engine never sees it.
- **Alternatives:** simdjson (faster, read-only, not needed at these sizes),
  RapidJSON (less convenient API).

### stb_image
- **Why:** decoding images without SDL keeps `assets` usable on the server and
  in tools. Decoding is restricted to formats we accept, and image dimensions
  are checked before decoding.
- **Risk:** stb_image has had security bugs with malicious files. Uploaded
  assets will be validated and re-encoded server-side in a sandbox before
  distribution (Milestone 6); clients will only receive platform-processed
  assets. See [security.md](security.md).

## Evaluated, planned

| Dependency | Version checked | License | Planned for | Notes |
|---|---|---|---|---|
| Luau | 0.743 | MIT | Milestone 2 scripting | CMake, C++17 (VM is C++11). Provides `luaL_sandbox`, an `interrupt` callback for execution limits, and custom allocators for memory limits. Optional native code generation (`Luau.CodeGen`) must stay off where JIT is not allowed (iOS) and in the browser. Has a `LUAU_BUILD_WEB` option. |
| Box2D | 3.2.0 | MIT | Rigid-body physics, when a game needs it | C, CMake ≥ 3.22. Will sit behind the `physics` module; see [ADR 0004](adr/0004-physics.md). |
| Dear ImGui | 1.93.0 | MIT | Milestone 3 Studio | Has an SDL3 + SDL_Renderer backend, so Studio can start on the same rendering path. Use the docking branch for a multi-panel editor. |
