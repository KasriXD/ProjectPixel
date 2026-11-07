# Security

All user-generated content (projects, scenes, assets and scripts) is
untrusted. This page lists what is protected today and what is planned;
[deployment.md](deployment.md) covers running a platform safely on the
internet.

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
| Eavesdropping on passwords, tokens and tickets | HTTPS (Mbed TLS) on the platform; clients check certificates against the system's store (or `PX_PLATFORM_CA`) and refuse untrusted ones; browsers reach game servers over `wss://` when the platform has a certificate | `third_party/CMakeLists.txt`, `online/PlatformClient.cpp`, `services/platform` (ADR 0025) |
| Script injection and clickjacking on the browser page | Strict Content Security Policy (scripts only from the platform, no inline scripts, `frame-ancestors 'none'`), `X-Frame-Options: DENY`, `nosniff` on every answer; game and account text inserted with `textContent` only; tested | `services/platform/web`, `PlatformService.cpp` |
| A link steering the browser player to another server with your ticket | The `/play` page builds the player's arguments itself and ignores arguments in its address (only a few test options are read, checked against patterns) | `services/platform/web/play.js`, `apps/player/web/pre.js` |
| Hostile WebSocket clients | The same message checks and limits as UDP; frames over 9 MiB refused; players limit per server | `net/WebSocketNative.cpp`, `GameServer` |
| Strangers watching or reaching players | Presence (online, playing what) only for friends; friendship needs the other's acceptance; limits of 200 friends and 100 open requests, rate limits on requests, invitations and profile changes | `services/platform/Social.cpp` (ADR 0027) |
| Unwanted contact, harassment | No free text between players; the only profile text is 200 characters, reportable, and administrators can clear it; portraits are 16 built-in pictures; blocking ends friendship, requests and invitations and keeps both people off the same game server; the blocked person is not told | `Social.cpp`, `Fleet::reserve` (avoid set), tested with real servers |
| Joining someone's game uninvited | Joining a friend needs the friendship, their setting (friends, or an invitation tied to their server, 10 minutes) and a game you may play; the ticket is the usual signed connect token | `Social.cpp`, tested |
| Links or other apps starting actions | On `/play`, `#play=` and `#join=` start the player only after a click on the page; Android takes only page and on-device options from other apps' intents | `web/play.js`, `PlayerActivity.java`, tested |
| Forged scores | Only authority scripts submit; servers send scores with their server key, only for players the platform seated on them and boards of the version they run; values finite and within ±10^15; owners and administrators can clear boards (audited) | `runtime/Leaderboards.cpp`, `services/platform/Leaderboards.cpp` (ADR 0028), tested |
| Hostile pictures | Game pictures (4 MiB at most) are decoded and scaled in the separate check process (512 MiB, 20 s), re-encoded as 320x180 PNG; only the result is stored and served | `online/Thumbnail.cpp`, `PlatformService.cpp`, tested |
| The saved login on a device | `login.json` readable only by its user (desktop), in the app's own folder on phones; tests never save one | `online/PlatformClient.cpp`, `apps/player/Launcher.cpp` |
| Eavesdropping on or tampering with game traffic | Every connection is encrypted and authenticated (X25519 key exchange, XChaCha20-Poly1305 per message, replay window for unreliable messages) on UDP and WebSockets alike; on the platform the ticket carries the game server's key and players refuse a server that cannot prove it holds it; a capture of a whole game shows no plaintext (CI) | `net/Secure.cpp`, `Fleet.cpp` (ADR 0030), `tests/test_secure.cpp`, `tools/net/capture_check.py` |
| A game exploiting the engine to take over the machine | Game servers and upload checks lock themselves down before running game code: no programs started, no new processes, no debugging, files read-only as listed (Landlock), TCP only to their own ports and the platform, no signals out; clean environment, no core dumps, limits on files and processes; a server that keeps a processor busy for 30 s is stopped ([below](#game-servers)) | `core/Sandbox.cpp`, `services/platform/Process.cpp`, `Fleet.cpp` (ADR 0031), `tests/test_sandbox.cpp` |
| Locked-out or abandoned accounts | Email addresses confirmed by a one-time link; password resets only to confirmed addresses, by one-time links (hashed, 1 hour), answering the same whether or not an account exists, rate-limited per address and name; a reset ends every session; deleting an account (with the password) removes its personal data and games | `services/platform/Accounts.cpp` (ADR 0032), `tests/test_accounts.cpp` |
| The mail server connection | SMTP only over TLS (or STARTTLS, then required), certificate and name checked; the password from the environment, never sent unencrypted; header injection refused | `services/platform/Mail.cpp`, tested against a fake server |
| A reset or confirmation link leaking | The `/play` page removes the token from the address before doing anything (history, screenshots); `Referrer-Policy: no-referrer`; tokens are stored only as hashes | `web/play.js`, `Accounts.cpp` |
| Clients hiding behind a proxy, or pretending to be one | `X-Forwarded-For` is only believed from `--trusted-proxy` addresses, read from the right; `/metrics` answers only locally and not through a proxy, unless a token is set | `services/platform/Operations.cpp`, tested |
| Losing the data | Online backups of the database with the files it refers to and the key, checked; restore checks integrity and every referenced file | `Operations.cpp` (ADR 0033), tested |

## Game servers

Each game server (`px-server`) and each upload check (`px-headless
--check-package`, `--check-thumbnail`) is started by the platform with:

- a **clean environment** (only `PATH`, `TZ`, certificate variables and its
  own key), no core dumps, at most 1024 open files and 256 MiB per written
  file (checks: 64 files, 512 MiB or 16 MiB), one process, the memory limit
  (`--server-memory`; checks 2 GiB, pictures 512 MiB);
- a **processor watch**: more than `--server-cpu` percent of a processor
  (default 100) in every sample for 30 s and the server is stopped;
- on Linux, after it has read the game and before it runs it, a
  **sandbox** it enters itself (`core/Sandbox.cpp`):
  - a **seccomp** filter: no `execve`, no `fork`/`vfork`/process `clone`,
    no `ptrace` or reading other processes' memory, no mounts, namespaces,
    kernel modules or keys, sockets only for UNIX, IPv4, IPv6 and netlink,
    signals only to itself;
  - **Landlock** (kernel 5.13+): reading only the system files it needs
    (libraries, certificates, time zones, name resolution) and its own
    inputs, writing nothing; with Landlock 4 (6.7+) TCP only on its own
    port and to the platform; with Landlock 6 (6.12+) no signals or
    abstract sockets out of the process.

The server's log says what it got, for example "Sandbox: system calls
filtered; Landlock 7: files read-only as listed, TCP ports limited, no
signals out". On Windows, a job object gives the one-process limit, UI
restrictions and no error dialogs; Windows and macOS have no system call
or file walls (production is expected on Linux). `--no-sandbox` turns the
lock-down off for debugging.

## Planned

- **Scripts**: per-tick total CPU budget for servers, fuzzing the script
  loader and bindings, keeping Luau current. Known gaps are listed in
  [scripting.md](scripting.md#what-the-sandbox-does-not-yet-protect-against).
- **HTTPS**: implemented with renewals taken into use without a restart
  ([deployment.md](deployment.md#certificates)). Still to do: HSTS once
  deployments always use HTTPS.
- **Game servers**: the sandbox above is Linux-only; one user account for
  all servers of a platform. Planned: separate user namespaces where
  available, per-server network bandwidth limits.
- **Multiplayer connections**: servers without `--platform` (LAN, Studio
  tests) encrypt but accept anyone and cannot be verified without
  `--server-key`. Planned: per-client message and bandwidth limits, kicking
  from the server.
- **Platform**: two-factor authentication, text filtering of titles,
  profiles and reports, parental settings (for example, friends only with a
  parent's approval).
- **Leaderboards**: they trust the game's own rules; detecting impossible
  scores (outliers, replays) is not done.
- **Assets**: fuzzing the image decoders with libFuzzer (uploads are
  already decoded only in the separate check process).
- **Fuzzing**: the project and scene parsers are good fuzzing targets
  (libFuzzer) and should be fuzzed before accepting uploads.
