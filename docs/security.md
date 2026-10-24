# Security

All user-generated content (projects, scenes, assets and, later, scripts) is
untrusted. This page lists what is protected today and what is planned.

## Implemented

| Threat | Protection | Where |
|---|---|---|
| Project files reading outside the project (`../`, absolute paths, drive letters, URLs) | Paths are normalized and rejected unless relative and inside the project | `core/FileSystem.cpp` `normalizeRelativePath` |
| Symbolic links pointing outside the project | Resolved path must stay inside the project root | `DirectoryFileSystem::readBinary` |
| Huge files | 64 MiB limit per file read | `IFileSystem::kMaxFileSize` |
| Decompression bombs (tiny file, huge image) | Dimensions read from the header and checked (8192x8192) before decoding | `assets/Image.cpp` |
| Unexpected image formats | stb_image built with PNG/JPEG/BMP/TGA only | `third_party/stb/stb_image_impl.c` |
| Resource exhaustion through scene data | Limits on entities (100 000), tilemap size (4096²), animation frames (1024), actions (32), assets (10 000) | `project/` |
| Malformed data reaching the engine | Strict parsing: types, ranges, unknown fields, references between entities and assets all validated at load | `project/` |
| A broken asset crashing a game | Missing/corrupt textures become a placeholder and are logged | `render/TextureCache.cpp` |
| Engine reading secrets | The engine has no credentials and makes no requests to network services; multiplayer traffic goes only between a game server and its clients | by design |
| Scripts accessing files, processes, network, clock | No `io`, `os`, `debug`, `require`, `load`/`loadstring`, `getfenv`/`setfenv`; only engine APIs | `scripting/ScriptVm.cpp`, tested |
| Scripts tampering with the engine or each other | Read-only built-in and engine tables and metatables, per-script environments, locked Entity metatable | `ScriptVm.cpp`, `Api.cpp`, tested |
| Scripts exhausting memory | Allocator cap per session (64 MiB); failing script disabled, garbage collected | `ScriptVm::allocate`, tested |
| Scripts hanging the game | Deterministic step budget per callback plus wall-clock watchdog; catching the error does not help | `ScriptVm::interrupt`, tested |
| Event storms, log spam | Per-tick limits on event deliveries, queued events and printed lines | `ScriptSystem`, tested |
| Malicious bytecode | Bytecode is only produced by the engine's compiler from source; never loaded from files | `ScriptLibrary` |
| Clients deciding game state | Authority-only API absent from client VMs; client scripts cannot write entity state | `Api.cpp`, tested (ADR 0011) |
| Multiplayer clients cheating | Clients send only their name and input bits; the server simulates everything, runs every script, and ignores input bits for undefined actions | `net/GameServer.cpp` (ADR 0018) |
| Malformed or hostile network messages | Every decoder checks bounds, lengths, counts against remaining bytes, finite floats and hierarchies; clients sending anything unexpected are disconnected; client messages over 2 KiB are refused; decoders are fuzzed with random and mutated messages under AddressSanitizer | `net/Bytes.cpp`, `net/Protocol.cpp`, `net/Replication.cpp`, `tests/test_net.cpp` |
| Clients holding server resources | Connections must say hello within 5 s; input queues are capped (128 frames, 512 ahead); player limit per server | `GameServer` |
| A server sending a client garbage | Clients validate snapshots like the server validates inputs and drop what does not decode | `GameClient` |

## Planned

- **Scripts**: per-tick total CPU budget for servers, fuzzing the script
  loader and bindings, keeping Luau current. Known gaps are listed in
  [scripting.md](scripting.md#what-the-sandbox-does-not-yet-protect-against).
- **Assets** (Milestone 6): uploads are validated and re-encoded by the
  platform in an isolated worker; clients receive only processed assets.
  Image decoders are fuzz-tested.
- **Game servers** (Milestone 6): each session runs in an isolated
  container with resource limits and no access to platform internals;
  servers authenticate to platform services with scoped, short-lived tokens.
- **Multiplayer connections** (Milestone 6): today anyone who can reach a
  server's UDP port can join it, names are not verified, and traffic is not
  encrypted; run servers only among people you trust. Planned: connect
  tokens issued by the platform's matchmaker (signed, short-lived, naming
  the player and session), encrypted transport, per-client message and
  bandwidth rate limits, and kicking or banning from the server.
- **Platform** (Milestone 6): authorization on persistent data, rate limits,
  reporting and moderation tooling, audit logs.
- **Fuzzing**: the project and scene parsers are good fuzzing targets
  (libFuzzer) and should be fuzzed before accepting uploads.
