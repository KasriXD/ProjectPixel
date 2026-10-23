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
| Luau | 0.743 (`9d7748a4`) | MIT (`third_party/luau/LICENSE.txt`; Lua 5.1 parts: `lua_LICENSE.txt`) | Vendored subset in `third_party/luau` (Common, Ast, Bytecode, Compiler, VM), see [ADR 0009](adr/0009-luau-integration.md) | `scripting` (private) |
| Dear ImGui | 1.92.9b, docking branch (`b48d1afb`) | MIT (`third_party/imgui/LICENSE.txt`) | Vendored in `third_party/imgui` (core, SDL3 platform backend, SDL_Renderer backend, `std::string` helpers), see [ADR 0013](adr/0013-studio-ui-dear-imgui.md) | Studio only (`apps/studio`) |
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

### Luau
- **Why:** the creator scripting language ([ADR 0008](adr/0008-scripting-luau.md)).
  Fast interpreter, gradual typing, designed for sandboxing untrusted code.
- **Maintenance:** actively developed by Roblox (weekly releases).
- **Platforms:** C++17, no platform code in the parts we use; verified on
  Linux, Windows (MSVC, MinGW), macOS and WebAssembly. Native code generation
  (`CodeGen`) is not vendored: no JIT on iOS or in browsers.
- **Build impact:** about 30 seconds of compilation on 2 cores; no network.

#### Updating Luau
1. `python3 tools/update_luau.py <tag>` (e.g. `0.744`). The script clones the
   tag, replaces the vendored directories, records tag and commit in
   `third_party/luau/VERSION` and regenerates `third_party/luau/sources.cmake`.
2. Rebuild and run the full test suite, including the sanitizer preset. The
   sandbox tests in `tests/test_scripting.cpp` fail if a new Luau version adds
   globals or libraries that must be reviewed.
3. Read the Luau release notes for VM and C API changes.
4. Update the version in this file and `third_party/VERSIONS.md`, commit.

### Dear ImGui
- **Why:** an immediate-mode UI that lets a small team build a dense,
  dockable editor quickly, with a maintained SDL3 + SDL_Renderer backend, so
  Studio draws through the same renderer as the game. Used only by Studio;
  games, px-player and the server never link it.
- **Version choice:** 1.92.9b (July 2026, a patch release) rather than
  1.93.0 (October 2026, nine days old when Milestone 3 started). The docking
  branch is the officially maintained variant with docking.
- **Fonts:** the embedded scalable ProggyForever font (MIT, inside
  `imgui_draw.cpp`); no font files are shipped.
- **Build impact:** about 20 seconds on 2 cores; `IMGUI_DISABLE_OBSOLETE_FUNCTIONS`
  is defined so removed APIs are caught at compile time.
- **Not used:** the multi-viewport feature (the SDL_Renderer backend does not
  support it) and `imgui_test_engine` (separate, non-MIT license).

#### Updating Dear ImGui
1. Read the "Breaking Changes" section of the new release's changelog.
2. `python3 tools/update_imgui.py <tag>-docking` (e.g. `v1.93.0-docking`); it
   replaces the vendored files and records tag and commit in
   `third_party/imgui/VERSION`.
3. Rebuild Studio, run the tests including `studio_smoke_headless_display`, and
   look at Studio (layout, fonts, docking, text input).
4. Update the version in this file and `third_party/VERSIONS.md`, commit.

## Evaluated, planned

| Dependency | Version checked | License | Planned for | Notes |
|---|---|---|---|---|
| Box2D | 3.2.0 | MIT | Rigid-body physics, when a game needs it | C, CMake ≥ 3.22. Will sit behind the `physics` module; see [ADR 0004](adr/0004-physics.md). |
