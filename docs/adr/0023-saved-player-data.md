# ADR 0023: Saved player data is loaded before joining and accessed synchronously

- Status: Accepted
- Date: 2026-11-04

## Context

Games need to remember things about players between visits (coins, levels,
unlocked items). Scripts run in a deterministic, fixed-tick simulation with
no coroutines, on authority only. Roblox-style data stores are asynchronous
(calls yield and can fail), which complicates every script that uses them.

## Decision

1. Saved data is a small **key-value map per player and game**
   (`runtime::PlayerData`): numbers, booleans, text and vectors, at most
   200 keys and 64 KiB.
2. A platform game server **loads a player's data before they join**
   (`IPlayerStore`, HTTP on a worker thread) and keeps it in the player's
   slot. Scripts read and write it **synchronously** with `Data.get`,
   `Data.set` and `Data.increment` (authority only); errors are ordinary
   script errors.
3. The server **saves** changed data every 30 s, when the player leaves
   (final save, which releases the player's lock), across level restarts and
   when the server stops; saves are retried and coalesced. A player whose
   data cannot be loaded is refused, never started with empty data.
4. Without a platform, data lives in memory (`SessionOptions::localData`);
   Studio keeps it between plays.

## Alternatives considered

- **Asynchronous calls in scripts** (callbacks or yields): more flexible
  (data of offline players, shared data) but every use becomes asynchronous
  error handling, and the VM has no coroutines today.
- **Saving the whole game state**: simple for single-player games, but
  meaningless for multiplayer servers that restart levels.

## Consequences

- Scripts stay simple and deterministic; tests can preload data.
- A crash between saves loses at most 30 s of progress; the lock keeps the
  older copy from being loaded elsewhere meanwhile (until the safety
  timeout).
- Not covered: data shared between players (leaderboards), data of offline
  players, migrations of a game's data format, export and deletion. These
  need new APIs, not changes to this one.
