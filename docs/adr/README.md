# Architecture Decision Records

| ADR | Title | Status |
|---|---|---|
| [0001](0001-language-build-and-naming.md) | C++20, CMake, and brand-neutral naming | Accepted |
| [0002](0002-dependency-management.md) | Dependency management | Accepted |
| [0003](0003-platform-layer-and-rendering.md) | SDL3 platform layer and the renderer abstraction | Accepted |
| [0004](0004-physics.md) | Own kinematic collision first, Box2D later | Accepted |
| [0005](0005-scene-model.md) | Scene model: own ECS with GUID identity | Accepted |
| [0006](0006-project-format.md) | Project format: versioned JSON with GUID references | Accepted |
| [0007](0007-deterministic-simulation.md) | Fixed-tick simulation driven by input frames, separate from presentation | Accepted |
| [0008](0008-scripting-luau.md) | Luau for creator scripting | Accepted, implemented |
| [0009](0009-luau-integration.md) | Vendor a minimal Luau subset | Accepted |
| [0010](0010-script-execution-model.md) | Script execution model: behaviours, order, failure policy, limits | Accepted |
| [0011](0011-script-realms.md) | Script realms and server authority | Accepted |
| [0012](0012-browser-runtime.md) | Browser runtime: feasibility and timing | Accepted |
| [0013](0013-studio-ui-dear-imgui.md) | Dear ImGui for Studio's UI, behind an editor model | Accepted |
| [0014](0014-component-descriptors.md) | Component descriptors as the single source of truth | Accepted |
| [0015](0015-studio-architecture.md) | Studio architecture: a UI-free editor model with snapshot undo | Accepted |
| [0016](0016-scene-hierarchy.md) | Scene hierarchy: parent links in the scene, computed world transforms | Accepted |
| [0017](0017-game-types-and-starter-library.md) | Game types and a starter library copied into projects | Accepted |
| [0018](0018-multiplayer-networking.md) | Multiplayer: authoritative server, input-only clients, delta snapshots | Accepted |
| [0019](0019-prefabs.md) | Prefabs: copies tracked by derived ids, updated by three-way merge | Accepted |
| [0020](0020-visual-scripting.md) | Visual scripting compiles graphs to Luau | Accepted |
| [0021](0021-platform-service.md) | The platform is a C++ service in this repository, on HTTP and SQLite | Accepted |
| [0022](0022-connect-tokens-and-game-sessions.md) | Signed connect tokens and platform-managed game sessions | Accepted |
| [0023](0023-saved-player-data.md) | Saved player data is loaded before joining and accessed synchronously | Accepted |
| [0024](0024-websockets-for-browsers.md) | Browsers join game servers over WebSockets | Accepted |
| [0025](0025-tls-with-mbed-tls.md) | HTTPS and secure WebSockets with Mbed TLS | Accepted |
| [0026](0026-mobile-apps.md) | Android and iOS apps from the same player | Accepted |
| [0027](0027-friends-presence-and-blocking.md) | Friends, presence and blocking in the platform | Accepted |
| [0028](0028-leaderboards.md) | Leaderboards declared by games, kept by the platform | Accepted |
| [0029](0029-launcher-in-the-player.md) | The launcher lives in px-player, drawn with Dear ImGui | Accepted |
| [0030](0030-encrypted-game-traffic.md) | Game traffic is encrypted end to end, with the server's key in the ticket | Accepted |
| [0031](0031-game-server-isolation.md) | Game servers and upload checks lock themselves down | Accepted |
| [0032](0032-accounts-and-email.md) | Accounts get email through the platform's own SMTP client | Accepted |
| [0033](0033-deployment-and-operations.md) | One container image; the platform does its own TLS, backups and metrics | Accepted |
| [0034](0034-audio.md) | Our own mixer on SDL's audio device; the platform stores sounds as QOA | Accepted |
| [0035](0035-bitmap-fonts.md) | Text uses bitmap fonts baked in Studio; players never read TrueType | Accepted |

New ADRs: copy [0000-template.md](0000-template.md).
