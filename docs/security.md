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
| Games reading secrets | Scripts have no network, file or credential access; only the programs (Studio, player, server) talk to the platform, with the user's or server's own token | by design |
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
| Stolen or guessed passwords | Argon2id (19 MiB, 2 passes, random salt); log-in rate limits per address and name; the same work for unknown names | `crypto/Crypto.cpp`, `services/platform` |
| Stolen session tokens | 256-bit random tokens stored only as BLAKE2b hashes; 30-day expiry; logging out deletes them; a ban deletes all of an account's | `services/platform` |
| Hostile uploads (crashing or exhausting the service, decoder exploits) | Checked in a separate `px-headless --check-package` process with memory, CPU and time limits; strict package decoder (fuzzed with mutations); every image re-encoded as PNG; only the checked package is stored and served | `online/PackageCheck.cpp`, `project/Package.cpp`, `services/platform` |
| Playing as someone else, joining without permission | Platform game servers require a connect token: EdDSA-signed, 60 s, single use, bound to the session; the name comes from the token | `net/ConnectToken.cpp`, `GameServer` (ADR 0022) |
| Servers reading or overwriting other players' data | A server's key only reaches the data of players the platform sent to it; one server holds a player's data at a time; data is never saved for a player whose data did not load | `services/platform/Fleet.cpp`, `GameServer` (ADR 0023) |
| Scripts filling the database | Saved data limits: 200 values, 1000-character text, 64 KiB per player and game; keys checked | `runtime/PlayerData.cpp` |
| Abuse of the API | Rate limits per address and account; request size limits; private and hidden games invisible to others | `services/platform` |
| Harmful games | Reports; administrators hide games (servers stop) and ban accounts; every action in the audit log | `services/platform`, Studio's Moderation tab |

## Planned

- **Scripts**: per-tick total CPU budget for servers, fuzzing the script
  loader and bindings, keeping Luau current. Known gaps are listed in
  [scripting.md](scripting.md#what-the-sandbox-does-not-yet-protect-against).
- **HTTPS**: the platform and its clients speak plain HTTP today; run the
  platform on one computer or a trusted network. Planned: TLS in the
  clients (and behind a TLS proxy on the server side).
- **Game servers**: today separate processes with a memory limit, running
  as the platform's account. Planned: isolated containers with CPU, memory
  and network limits and no access to the platform's files.
- **Multiplayer connections**: game traffic is authenticated at joining
  (platform servers) but not encrypted; servers without `--platform` (LAN,
  Studio tests) still accept anyone. Planned: keys in the connect token and
  encrypted transport, per-client message and bandwidth limits, kicking
  from the server.
- **Platform**: email verification and password reset, two-factor
  authentication, account deletion, text filtering of titles and reports.
- **Assets**: fuzzing the image decoders with libFuzzer (uploads are
  already decoded only in the separate check process).
- **Fuzzing**: the project and scene parsers are good fuzzing targets
  (libFuzzer) and should be fuzzed before accepting uploads.
