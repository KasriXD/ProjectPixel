# ADR 0021: The platform is a C++ service in this repository, on HTTP and SQLite

- Status: Accepted
- Date: 2026-11-04

## Context

Milestone 6 adds the platform: accounts, publishing with versions, a
catalog, game servers started on demand, saved player data and moderation.
It must check uploaded games with exactly the rules the engine applies,
start game servers, be testable in CI on Linux, Windows and macOS like
everything else, and be runnable by one person on one computer today while
leaving a path to a hosted service.

## Decision

1. **One C++ program, `px-platform`, in this repository** (`services/platform`),
   built and tested with the engine. It reuses the engine's own code: the
   package format, the game loader for upload checks, connect tokens, the
   saved data type.
2. **HTTP + JSON** API (`/api/v1`), served by cpp-httplib (vendored, plain
   HTTP). Clients use the same library through `online::PlatformClient`, and
   the wire types and their JSON live in one header (`online/WireJson.hpp`)
   shared by both sides.
3. **SQLite** (vendored amalgamation) in WAL mode, one connection behind a
   mutex; slow work (Argon2, upload checks, waiting for a server) happens
   outside the lock. The schema is versioned with `PRAGMA user_version`.
4. **Untrusted content runs in child processes**, never in the service:
   uploads are checked by `px-headless --check-package` with memory, CPU and
   time limits; games run in `px-server` processes. A small cross-platform
   `Process` class (fork/exec with rlimits; CreateProcess with a job object)
   starts them with no inherited handles.
5. **Packages are content-addressed** (BLAKE2b-256) and stored as files;
   versions refer to them by hash.
6. The service is a library (`px_platform_service`) so tests run it
   in-process on a free port, with real px-headless and px-server children.

## Alternatives considered

- **A separate web stack (TypeScript or Go with PostgreSQL).** Conventional
  for web services and easier to scale out, but adds a second language and
  toolchain, and upload checks would still have to call the engine. Chosen
  against by the founder for this milestone.
- **PostgreSQL now.** Needs a server to install and run for development and
  CI; SQLite is enough for one machine and the SQL is portable.
- **gRPC or a binary protocol.** Less inspectable and harder for web pages
  and future third-party tools than JSON over HTTP.

## Consequences

- `cmake --build` gives a complete, local platform; the whole loop (publish,
  join, play, save) is covered by tests on every CI platform.
- One process and one database file limit the scale (a small community).
  Moving to PostgreSQL and several service instances behind a load balancer
  means replacing `Database` and moving the fleet's in-memory state (seats,
  player locks) into shared storage.
- No HTTPS yet: cpp-httplib needs OpenSSL for TLS, which the clients do not
  ship. Until then the platform is for one computer or a trusted network
  (docs/platform.md#security).
