# ADR 0018: Multiplayer: authoritative server, input-only clients, delta snapshots

- Status: Accepted
- Date: 2026-10-24

## Context

Milestone 4 adds multiplayer: a dedicated server built from the headless
runtime, a transport, sessions with players joining and leaving, and
replication. Constraints from earlier decisions:

- The server is the authority over game state (ADR 0011). Creators must
  not have to write netcode for ordinary games, and cheating by modified
  clients must be hard by design.
- The simulation runs in fixed ticks from input frames and is deterministic
  on one build, but not bit-identical across compilers and CPUs (ADR 0007).
- The browser must remain a possible client (ADR 0012): browsers cannot send
  UDP datagrams.
- Content is data loaded from a project folder; scripts are Luau run in a
  sandbox (ADRs 0006, 0008).

## Decision

1. **Server-authoritative simulation, input-only clients.** The server runs
   the only real `GameSession`. A client sends who it is (once) and its
   input frames (every tick: the held and pressed action bits). It never
   sends positions, scores or any other state, so there is nothing for a
   modified client to lie about except its own key presses. Scripts run
   only on the server.
2. **Snapshot replication with per-field deltas.** After a tick the server
   captures the replicated part of the world (a `WorldState`) and sends each
   client the difference from the newest state that client acknowledged,
   unreliably, 30 times a second by default (Quake 3's model). A lost
   snapshot needs no resend: the next one is a difference from an older
   acknowledged state. The baseline of last resort is the initial scene,
   which both sides load from the same project; a hash of it (with the
   engine and protocol versions, input actions and tick rate) is compared
   when a client joins, so a different copy of the game is refused with a
   clear message instead of desynchronising.
3. **Generic replication from the component descriptors** (ADR 0014). A
   table names the components that replicate (Transform, Sprite, Tilemap,
   BoxCollider, KinematicBody, the controllers, Player, Tags) and the few
   field exceptions; every field of those is encoded by kind. New fields
   replicate without network code. Cameras, animation state (its result,
   `Sprite.source`, is sent) and scripts stay local.
4. **Clients hide latency for their own character only.** Other entities are
   drawn about 100 ms in the past, blended between two snapshots. The own
   character is predicted by running the shared movement code
   (`runtime/Movement.hpp`, the same functions the server calls) on the
   inputs the server has not confirmed yet; when a snapshot arrives the
   prediction restarts from the server's state and replays them, and any
   difference is faded out over 0.1 s.
5. **Players are copies of the scene's Player entities.** In a multiplayer
   session, entities with a `Player` component are templates and spawn
   points; each player who joins gets a copy with deterministic ids. Each
   character reads its own player's input; scripts get `PlayerJoined`,
   `PlayerLeft`, `Players`, `Input.isDown(action, player)`. Score and
   objective stay shared (co-operative) for now.
6. **A message transport behind an interface.** `INetEndpoint` offers
   reliable-ordered and unreliable messages and nothing UDP-specific. Two
   implementations exist: UDP through ENet 1.3.18 (desktop; reliable
   handshake channel, unreliable channel with unreliable fragments for large
   snapshots) and an in-process loopback network with simulated latency,
   jitter, loss and duplication (tests and the `--net-test` tool). A
   WebSocket or WebTransport endpoint for browsers can be added without
   touching the protocol: over a reliable stream, "unreliable" messages are
   simply always delivered.
7. **Programs.** `px-server` is the dedicated server; `px-player --connect`
   joins one; `px-player --host` runs a server in the window for quick games
   among friends; Studio's *Test Multiplayer* starts a server and player
   windows locally.

## Alternatives considered

- **Deterministic lockstep** (clients exchange inputs and all simulate):
  tiny bandwidth, but needs bit-exact determinism across platforms (we do
  not have it), makes every player wait for the slowest, and gives every
  client the whole game state and logic.
- **Client authority over the own character** (Roblox's network ownership):
  instant movement without prediction code, but speed and teleport cheats
  need server-side heuristics, and it breaks the "clients send only input"
  rule ADR 0011 committed to.
- **Full snapshots without deltas:** simpler, but bandwidth grows with the
  whole world every tick instead of with what changed.
- **Hand-written replication per component:** precise, but every new field
  would need network code; the descriptors already describe everything.
- **GameNetworkingSockets or yojimbo/netcode.io:** encryption and
  authentication built in, but heavy dependencies (protobuf, OpenSSL or
  libsodium), harder Windows/mobile builds, and still no browser support.
  Authentication belongs with the platform services (Milestone 6), which
  can issue connect tokens to any transport.
- **WebRTC data channels or WebTransport now:** would serve browsers, but
  are large to integrate and run on servers; there is no browser client to
  use them yet (ADR 0012).

## Consequences

- Measured on the examples: about 3 kB/s from the server to each client and
  under 2 kB/s back; idle snapshots are tens of bytes. A three-player
  top-down level was completed through a simulated 60 ms link with 15 ms
  jitter and 5 % loss with one or two visible corrections.
- Moving platforms, springs and other scripted pushes on the own character
  are corrected after a round trip (a small visible snap); remote entities
  are drawn slightly in the past. Both are the usual costs of this model.
- Every client receives every entity: fine for levels of hundreds of
  entities, but large worlds will need interest management (relevance by
  distance) before Milestone 8's persistent worlds.
- There is no encryption, authentication or flood protection yet; servers
  should run only for trusted players until the platform's connect tokens
  exist (docs/security.md).
- Client-side scripts (presentation), client-to-server requests that server
  scripts validate, per-player state in the API (score, inventory) and
  browser clients are follow-ups; the protocol has room for new message
  types and a version number to change it.
