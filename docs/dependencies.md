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
| Dear ImGui | 1.92.9b, docking branch (`b48d1afb`) | MIT (`third_party/imgui/LICENSE.txt`) | Vendored in `third_party/imgui` (core, SDL3 platform backend, SDL_Renderer backend, `std::string` helpers), see [ADR 0013](adr/0013-studio-ui-dear-imgui.md) | Studio and px-player's launcher ([ADR 0029](adr/0029-launcher-in-the-player.md)); never games |
| ENet | 1.3.18 (`2662c0de`) | MIT (`third_party/enet/LICENSE`) | Vendored unmodified in `third_party/enet` with our own CMake target; not built for WebAssembly (`PX_ENABLE_ENET`) | `net` (private): the UDP transport |
| cpp-httplib | 0.62.0 (`74ef3c42`) | MIT (`third_party/httplib/LICENSE`) | Vendored in `third_party/httplib`, split by its own `split.py` into `httplib.h` + `httplib.cc`; TLS through Mbed TLS | `online` (client: desktop and phones), `services/platform` (server), `net` (WebSockets); not in the browser |
| Mbed TLS | 3.6.7 | Apache-2.0 | Downloaded release tarball, SHA-256 pinned, static libraries; only with the platform (`PX_ENABLE_TLS`) | HTTPS and `wss://` in cpp-httplib ([ADR 0025](adr/0025-tls-with-mbed-tls.md)) |
| SQLite | 3.54.1 (`57520227`) | Public domain | Amalgamation (`sqlite3.c`/`sqlite3.h`) built from the official source tag with its own `make sqlite3.c`, vendored in `third_party/sqlite` | `services/platform` |
| Monocypher | 4.0.3 (`ab2b16dd`) | BSD-2-Clause or CC0 (`third_party/monocypher/LICENCE.md`) | Vendored `monocypher.c`/`.h` | `crypto` (private): Argon2id, EdDSA, BLAKE2b |
| stb_image_write | 1.16, commit `a7d4c217` | MIT / public domain | Vendored in `third_party/stb`, compiled in one C file, PNG to memory only | `online` (private): re-encoding uploaded images |
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
  Studio draws through the same renderer as the game. Used by Studio and by
  px-player's launcher (desktop and phones, not the browser player); games
  never use it, and px-server, px-headless and the WebAssembly player never
  link it.
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
   `third_party/imgui/UPSTREAM_VERSION.txt`. (Not `VERSION`: the folder is an
   include path, and on case-insensitive file systems such as macOS's a file
   named `VERSION` is picked up by `#include <version>`.)
3. Rebuild Studio, run the tests including `studio_smoke_headless_display`, and
   look at Studio (layout, fonts, docking, text input).
4. Update the version in this file and `third_party/VERSIONS.md`, commit.

### ENet
- **Why:** the UDP transport for multiplayer ([ADR 0018](adr/0018-multiplayer-networking.md)):
  connections, reliable ordered and unreliable messages on separate
  channels, fragmentation (including unreliable fragments for large
  snapshots), round-trip and loss estimates. Small (about 6000 lines of C),
  no dependencies, used by many games for two decades; builds with MSVC,
  MinGW, GCC, Clang and Apple Clang. It sits behind `INetEndpoint`, so it
  can be replaced without touching the protocol.
- **Version:** 1.3.18 (April 2024), the latest release; the project changes
  slowly and the release is stable.
- **Alternatives:** GameNetworkingSockets (encryption and NAT traversal, but
  protobuf and OpenSSL/libsodium dependencies and heavy builds),
  yojimbo/netcode.io (connect tokens and encryption, libsodium and mbedtls),
  SDL_net (no reliability layer), raw sockets (reliability and
  fragmentation by hand). None of them work in browsers either; a browser
  client will use a WebSocket/WebTransport endpoint.
- **Not provided:** encryption and authentication. Platform servers
  authenticate players with connect tokens (ADR 0022); encryption is
  planned ([security.md](security.md)).
- **Vendoring:** the sources are copied unmodified; as for the other
  vendored libraries, the build file (`third_party/enet/CMakeLists.txt`) is
  ours: one static target, the platform checks ENet's own build performs
  (`poll`, `getaddrinfo`, ...), Winsock on Windows. Update by copying the `.c` files, `include/enet/` and
  `LICENSE` of a new tag, then run the networking tests on every platform.
- **Build impact:** a few seconds.

### cpp-httplib
- **Why:** a small, widely used HTTP/1.1 server and client in two files,
  enough for the platform's JSON API and package downloads, with a thread
  pool on the server side. Works on Linux, macOS and Windows 10+ (it needs
  `_WIN32_WINNT=0x0A00`, set by our CMake target).
- **Alternatives:** Boost.Beast (large dependency), libcurl + a separate
  server library (two libraries, libcurl's build), civetweb (C, server only).
- **TLS:** through Mbed TLS (below), with the system's certificates on each
  OS (Android: the system's certificate folder; iOS: a certificate file
  built into the app, `PX_CA_BUNDLE`). Also provides the WebSocket server and client for browser players
  ([ADR 0024](adr/0024-websockets-for-browsers.md)).
- **Update:** clone a tag, run `python3 split.py -o out`, copy `out/httplib.h`,
  `out/httplib.cc` and `LICENSE`, run the platform tests on every OS.

### Mbed TLS
- **Why:** TLS for HTTPS and secure WebSockets that builds the same way on
  Windows, macOS and Linux, with no system library to install; cpp-httplib
  supports it directly. The platform's SMTP client uses it too
  (`Mail.cpp`, [ADR 0032](adr/0032-accounts-and-email.md)), and the
  certificate checks for renewals. Built with its programs and tests off.
- **Alternatives:** OpenSSL (not on Windows by default, large build), the
  systems' own TLS (Schannel, Secure Transport, OpenSSL on Linux: three
  implementations). See [ADR 0025](adr/0025-tls-with-mbed-tls.md).
- **Update:** change the URL and SHA-256 in `cmake/PxDependencies.cmake`;
  follow its security advisories (3.6 is a long-term support branch).
- **Build impact:** about 30 seconds.

### SQLite
- **Why:** an embedded, transactional database in one C file; nothing to
  install for development, CI or a small platform. Compiled with
  `SQLITE_THREADSAFE=2`, foreign keys on, no extension loading.
- **Alternatives:** PostgreSQL (a server to run; the likely next step when
  the platform outgrows one machine, see ADR 0021).
- **Update:** clone the `version-X.Y.Z` tag of https://github.com/sqlite/sqlite,
  run `./configure && make sqlite3.c` (needs tclsh), copy `sqlite3.c` and
  `sqlite3.h`.

### Monocypher
- **Why:** audited, small (one C file), no dependencies, portable to every
  target including WebAssembly; covers exactly what the platform needs:
  Argon2id for passwords, EdDSA for connect tokens, BLAKE2b for hashes,
  X25519 and XChaCha20-Poly1305 for game traffic
  ([ADR 0030](adr/0030-encrypted-game-traffic.md)). Random numbers come
  from the operating system (`crypto/Crypto.cpp`).
- **Alternatives:** libsodium (larger, separate build), OpenSSL (large, hard
  to ship on every platform).

### Tools used only by tests and deployment

Not part of any program: Playwright with headless Chromium (browser tests,
`tools/browser`), aiosmtpd (a real SMTP server for the accounts test),
certbot (the Compose file's certificates; its own image), Ubuntu 24.04 (the
container image's base).

## Evaluated, planned

| Dependency | Version checked | License | Planned for | Notes |
|---|---|---|---|---|
| Box2D | 3.2.0 | MIT | Rigid-body physics, when a game needs it | C, CMake ≥ 3.22. Will sit behind the `physics` module; see [ADR 0004](adr/0004-physics.md). |
