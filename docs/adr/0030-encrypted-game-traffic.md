# ADR 0030: Game traffic is encrypted end to end, with the server's key in the ticket

- Status: Accepted
- Date: 2026-11-07

## Context

Since Milestone 5 a platform game server checks a signed connect token
before a player joins (ADR 0022), but everything after that travelled in
the clear over UDP and `ws://`: anyone on the path could read inputs and
snapshots, inject messages into a session or replay them. Only browsers on
an HTTPS page were protected, by `wss://` (ADR 0025), and only up to a
server presenting the platform's certificate. Milestone 9 puts the platform
on the internet, where the path between a phone on public Wi-Fi and a game
server is not trusted.

Requirements: the same protection over every transport (ENet UDP,
WebSockets, the loopback used in tests); no certificates on game servers
(they start and stop all the time, on ports not names); unreliable messages
stay unreliable (a lost snapshot must not stall the next); small overhead
on messages that are often a few dozen bytes; no new dependency.

## Decision

1. **A secure layer between the transport and the game protocol**
   (`net/Secure.cpp`), as an `INetEndpoint` decorator. `GameServer` and
   `GameClient` always wrap their endpoint in it; nothing above or below
   changes.
2. **Handshake**: the client sends a fresh X25519 key (ClientHello); the
   server answers with its own fresh key and, on platform servers, a
   long-lived identity key (ServerHello). Keys for each direction come from
   a BLAKE2b hash of the whole handshake and the shared secrets (keyed
   BLAKE2b). Every message after that is XChaCha20-Poly1305 with a 64-bit
   counter as the nonce: 25 bytes per message (kind, counter, tag).
   Reliable and unreliable messages count separately; unreliable ones are
   checked against a 128-message replay window, so loss and reordering are
   fine. Monocypher (already vendored for passwords and signatures)
   provides all of it.
3. **Who the server is**: the platform makes an identity key for every game
   server it starts (`PX_SERVER_IDENTITY`, in the environment, never on the
   command line) and puts its public half in the join ticket (`serverKey`).
   The client refuses a server that cannot prove it holds that key
   (`ServerUnverified`). Servers started by hand still encrypt, but a
   client that was not given `--server-key` cannot tell who it talks to.
4. **Versions**: protocol version 3. Older players get a plaintext Reject
   ("update your player") they can read; a client meeting an older server
   reports a version mismatch rather than garbage.
5. **The server side does not wait**: its secure layer reports a
   connection at once and the game server's own Hello timeout (game time,
   not wall-clock) covers a client that never finishes the handshake, so
   tests on simulated networks stay deterministic.

## Alternatives considered

- **DTLS / TLS for every transport**: certificates on short-lived servers
  identified by port, and a second TLS stack for UDP (Mbed TLS has DTLS,
  but ENet would have to sit on top of it, losing its own reliability
  layer, or under it, needing a custom BIO). Much more code and overhead
  for the same guarantee the ticket's key already gives.
- **Noise (a library)**: the right shape, but another dependency for what
  is a two-message handshake with primitives already in the tree.
- **Encrypting only on the platform**: servers started by hand (LAN,
  Studio tests) would be the odd ones out; encrypting always keeps one code
  path and makes packet captures useless everywhere.

## Consequences

- A packet capture of a whole game shows no plaintext (checked in CI with
  `tools/net/capture_check.py`).
- 25 bytes more per message: about 4 % of a typical snapshot stream.
- Forward secrecy per connection (fresh keys each time); the identity key
  lives as long as its game server.
- `ws://` traffic is now protected too; `wss://` adds the platform's
  certificate on top, which browsers need anyway on an HTTPS page.
- Not covered: traffic analysis (sizes and timing), and a malicious game
  server the platform itself started (it holds the key it was given).
