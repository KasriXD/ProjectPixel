# ADR 0028: Leaderboards declared by games, kept by the platform

- Status: Accepted
- Date: 2026-11-05

## Context

Games want to compare players: fastest times, highest scores. Scores must
be shared by everyone who plays a game (on any server, in any app), must
not be forgeable by clients, and scripts must be able to read them during a
tick without waiting for the network (ADR 0023 made the same choice for
saved player data).

## Decision

1. **Declared in `project.json`**: up to 8 boards with an id, a title,
   which score is best (`highest`/`lowest`) and a display format
   (`number`/`time`). The platform reads them from each published version's
   check report, so it knows a game's boards without running it.
2. **Submitted by authority scripts only** (`Leaderboard.submit`,
   `best`, `rank`, `top`): the group exists only where scripts have
   authority (alone, or on the server), like the rest of the
   authority-only API (ADR 0011). Clients cannot submit.
3. **One best per account, game and board** on the platform (an upsert that
   keeps the better value), not every score: small, and what players look
   at. Ranks are competition ranks.
4. **Stores behind one interface** (`runtime::ILeaderboardStore`): a memory
   store when playing alone or on a server without a platform, and on
   platform servers a store with a memory copy (top 100 per board and the
   bests of the players there) refreshed every 15 s, sending new bests on a
   worker thread and before the server stops. Scripts always read the copy
   synchronously.
5. **Servers send scores with their server key**, and only to boards of the
   version they run, for accounts the platform seated on them; values must
   be finite and within ±10^15.
6. **Owners and administrators can clear** a board or one player's score
   (audited); players see boards on the game's page (everyone, or friends).

## Alternatives considered

- **Clients submit with their own token**: trivial to cheat.
- **Every score kept** (history, periods): more useful queries later, much
  more data; the best is enough for the first boards.
- **Asking the platform on each script call**: up-to-date, but a network
  round trip inside a tick, or callbacks scripts would have to handle.
- **Boards created through the API** instead of the project: a second place
  to keep in step with the game; in the project they travel with the game
  and are checked like the rest.

## Consequences

- `Leaderboard.top` on one server can lag other servers by up to 15 s.
- Boards are as fair as the game's scripts: the platform cannot know
  whether a time was earned.
- Changing a board's order in a new version leaves old bests chosen the
  other way; owners clear the board.
- Later: periods (daily, weekly), per-version boards, a Studio editor,
  showing boards inside games.
