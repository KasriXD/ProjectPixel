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
| Engine reading secrets | The engine has no credentials and makes no network requests | by design |
| Scripts accessing files, processes, network, clock | No `io`, `os`, `debug`, `require`, `load`/`loadstring`, `getfenv`/`setfenv`; only engine APIs | `scripting/ScriptVm.cpp`, tested |
| Scripts tampering with the engine or each other | Read-only built-in and engine tables and metatables, per-script environments, locked Entity metatable | `ScriptVm.cpp`, `Api.cpp`, tested |
| Scripts exhausting memory | Allocator cap per session (64 MiB); failing script disabled, garbage collected | `ScriptVm::allocate`, tested |
| Scripts hanging the game | Deterministic step budget per callback plus wall-clock watchdog; catching the error does not help | `ScriptVm::interrupt`, tested |
| Event storms, log spam | Per-tick limits on event deliveries, queued events and printed lines | `ScriptSystem`, tested |
| Malicious bytecode | Bytecode is only produced by the engine's compiler from source; never loaded from files | `ScriptLibrary` |
| Clients deciding game state | Authority-only API absent from client VMs; client scripts cannot write entity state | `Api.cpp`, tested (ADR 0011) |

## Planned

- **Scripts**: per-tick total CPU budget for servers, fuzzing the script
  loader and bindings, keeping Luau current. Known gaps are listed in
  [scripting.md](scripting.md#what-the-sandbox-does-not-yet-protect-against).
- **Assets** (Milestone 6): uploads are validated and re-encoded by the
  platform in an isolated worker; clients receive only processed assets.
  Image decoders are fuzz-tested.
- **Game servers** (Milestones 4, 6): each session runs in an isolated
  container with resource limits and no access to platform internals;
  servers authenticate to platform services with scoped, short-lived tokens.
- **Authority** (Milestone 4): the server decides all important outcomes
  (damage, rewards, inventory, purchases). Clients send input, not results.
- **Platform** (Milestone 6): authorization on persistent data, rate limits,
  reporting and moderation tooling, audit logs.
- **Fuzzing**: the project and scene parsers are good fuzzing targets
  (libFuzzer) and should be fuzzed before accepting uploads.
