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

## Planned

- **Scripts** (Milestone 2): Luau sandbox, no OS or native access, CPU and
  memory budgets, server-only APIs absent from client VMs. See
  [scripting.md](scripting.md).
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
