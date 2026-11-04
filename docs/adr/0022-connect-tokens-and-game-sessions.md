# ADR 0022: Signed connect tokens and platform-managed game sessions

- Status: Accepted
- Date: 2026-11-04

## Context

Until now anyone who could reach a game server's UDP port could join it
under any name (ADR 0018). Platform games need: only players the platform
sent, with their real account names; servers started when needed and
stopped when empty; and each player's saved data held by one server at a
time.

## Decision

1. **Connect tokens**: on `POST /games/{id}/join` the platform picks a seat
   and signs `pxct1 | session | account id | name | expiry | nonce` with its
   EdDSA key (Monocypher). The token travels in the Hello message (protocol
   version 2). A platform server (started with `--platform-key`) checks the
   signature, its own session id, the expiry (60 s), that the nonce was not
   seen, and that the account is not already playing there; the player's
   name comes from the token.
2. **Sessions**: the platform's fleet finds a running server of the game's
   latest version with room (players plus reserved seats) or starts a
   `px-server` process on a free port, passing the session id, the public
   key and (in the environment) a random server key for its HTTP calls. The
   join call waits until the server reports it is ready. Servers send a
   heartbeat with the players' account ids every 5 s and stop themselves
   when empty; the platform kills servers that are silent for 30 s.
3. **One server per player**: a reserved seat locks the player to that
   session until their final save arrives (or the seat expires unused, or
   the server ends). Joining another game meanwhile is refused; joining the
   same game again returns the same server.
4. Servers without `--platform` keep the open behaviour of ADR 0018.

## Alternatives considered

- **Servers asking the platform about each joining player.** Simpler tokens
  (random), but every join depends on the platform answering; signed tokens
  let a server decide alone and later run anywhere.
- **JWT.** A general format with algorithm negotiation and many pitfalls;
  six fixed fields signed with one algorithm are smaller and stricter.
- **Netcode.io-style tokens with encryption keys.** Planned (encrypted game
  traffic) but needs HTTPS to deliver keys safely; the token format has a
  version prefix for it.

## Consequences

- Names in platform games are verified; tickets cannot be reused or moved to
  another server; a leaked ticket is useless after 60 seconds.
- Players cannot be in two games at once (by design, for saved data).
- The platform's private key is a critical secret; rotating it needs a
  restart of running servers (they are given the public key at start).
- Seat filling is first come, first served; parties and choosing a server
  come later.
