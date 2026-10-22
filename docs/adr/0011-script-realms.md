# ADR 0011: Script realms and server authority

- Status: Accepted (authority realm implemented; client realm implemented in
  the scripting module and tested, not yet used by a networked client)
- Date: 2026-10-23

## Context
Multiplayer will be server-authoritative: clients must not decide damage,
rewards, inventory, purchases or progress. If scripts written now can
freely change game state from anywhere, content will depend on that and
multiplayer will either be insecure or break existing games.

## Decision
- Every script VM has a **realm**: `Authority` (single-player session today,
  dedicated server later) or `Client`.
- Every API member is marked **all** or **authority** in the API table.
  Authority-only functions are not registered at all in client VMs (they are
  `nil`), and client scripts cannot assign entity properties or destroy
  entities. Enforcement is in the bindings, not in documentation.
- State-changing operations go through `IScriptHost`, which the runtime
  implements; on a server this is where validation and replication hook in.
- Each VM has its own event bus; nothing crosses the network implicitly.
  Event values are restricted to serializable types now, so explicit network
  messages can reuse them later.
- Input is read through actions (`Input.isDown`), never devices, so a server
  can supply each player's input frames.

## Planned (Milestone 4)
- A per-behaviour setting for where it runs (`server`, `client`, `both`).
- Client-to-server requests that server scripts validate (RemoteEvent-like),
  rate-limited.
- Per-player input and player handles on the server.
- Client-local entities for effects and UI that clients may modify.

## Consequences
- Single-player content written today runs unchanged as server content.
- Client-side prediction and presentation scripts will need the planned
  client-local entity concept, because client VMs cannot change replicated
  entities.
- Tests create a client-realm VM and check that authority functions are
  absent and writes are refused.
