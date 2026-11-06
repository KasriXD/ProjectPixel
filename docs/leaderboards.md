# Leaderboards

A game declares leaderboards in its project, its scripts submit scores, and
the platform keeps each player's best, shared by everyone who plays the
game. Players see them on the game's page in the launcher and the browser
([launcher.md](launcher.md)). Design: [ADR 0028](adr/0028-leaderboards.md).

**Status:** implemented. Unit tests cover the declarations, the script API,
the stores and the platform's API; a test with real game server processes
sends scores to the platform and reads them back in a server script; the
examples' best times reach the platform's leaderboards in the launcher and
browser checks in CI.

## Declaring a leaderboard

In `project.json` ([project-format.md](project-format.md)), up to 8:

```json
"leaderboards": [
  { "id": "fastest", "title": "Fastest coin run", "order": "lowest", "format": "time" },
  { "id": "coins", "title": "Most coins" }
]
```

| Field | | |
|---|---|---|
| `id` | required | what scripts use: 1-32 of `a-z`, `0-9`, `_`; unique |
| `title` | required | shown to players, up to 40 characters |
| `order` | `highest` (default) or `lowest` | which score is best |
| `format` | `number` (default) or `time` | how scores are shown: `1234`, `3.50`; times in seconds as `1:02.35` |

Studio keeps the list when it saves; edit it in the file for now.

## Submitting scores

Scripts that run with authority (alone, or on the server in multiplayer)
use the `Leaderboard` group ([scripting-api.md](scripting-api.md#leaderboard)):

```lua
-- each player's time when the level is complete (examples' best_time.luau)
Events.on("level_complete", function()
    for _, player in Players.getAll() do
        Leaderboard.submit(player, "fastest", Game.time())
    end
end)
```

- `Leaderboard.submit(player, board, score)` keeps the score if it is the
  player's best and returns whether it was. Scores must be finite numbers;
  at most 100 submissions per tick.
- `Leaderboard.best(player, board)` and `Leaderboard.rank(player, board)`:
  the player's best and place (nil without a score).
- `Leaderboard.top(board, count)`: the best `count` (default 10, at most
  100) as `{rank, name, score}` tables.

Places follow competition ranking: equal scores share a place and the next
place is skipped (1, 2, 2, 4).

The Toolbox's **Best Time** behaviour (`scripts/best_time.luau`) does this
for a time board named `fastest`; both examples use it.

## Where scores are kept

| Played | Scores |
|---|---|
| Alone (px-player, Studio's play mode) | in memory until the program closes |
| On a server without a platform (`px-server`, `--host`) | in the server's memory, for everyone on it, until it stops |
| On the platform's game servers | on the platform: each account's best per game and board, kept across versions and servers |

Platform game servers keep a copy in memory so scripts get answers at once:
the top 100 of each board and the bests of the players on the server,
refreshed every 15 seconds. New bests go to the platform on a worker thread
and are sent before the server stops. So `Leaderboard.top` on a server may
be up to 15 seconds behind other servers; a player's own best is always up
to date on their server.

## For owners and administrators

`DELETE /api/v1/games/{id}/leaderboards/{board}` removes every score of a
board, `.../{board}/{username}` one player's (owners and administrators,
logged in the audit log). Use it after a cheat, or when a new version
changes what a score means. A board removed from `project.json` disappears
from the game's page; its scores stay unless removed.

## HTTP API

| Method and path | Auth | |
|---|---|---|
| `GET /games/{id}/leaderboards?limit=` | (Bearer) | each board of the latest version: `{id, title, order, format, top, you?, friends}`; `top` the best `limit` (default 10, at most 100); with a login, your entry and you with your friends |
| `DELETE /games/{id}/leaderboards/{board}[/{username}]` | Bearer (owner, admin) | remove scores → `{removed}` |
| `POST /server/leaderboards` | Server | `{scores: [{userId, board, value}]}`, up to 200; only boards of the version the server runs, finite values within ±10^15; the platform keeps each best |
| `GET /server/leaderboards?limit=&users=` | Server | the boards (top entries with account ids) and the bests of these accounts |

## Limitations

- Scores come from the game's own scripts on the game servers: they are as
  trustworthy as the game's rules. Clients cannot submit (the API exists
  only with authority), but a game that hands out points carelessly gets
  careless boards.
- No periods (daily, weekly) and no per-version boards yet.
- If a new version flips a board's `order`, the stored bests were chosen
  with the old order: clear the board.
- No editor for leaderboards in Studio yet.
