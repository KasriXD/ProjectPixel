# The platform

The platform is where games are published and played: accounts, a catalog
of games, game servers it starts on demand, saved player data and
moderation. It is one program, **px-platform**, with an HTTP API that
Studio, the player, game servers and tools use. This is the first version
([ADR 0021](adr/0021-platform-service.md)): it runs well on one computer or
a small server, and its limits are listed at the end.

```
 Studio ──publish──▶ ┌──────────────┐ ◀──log in, browse, join── px-player
                     │ px-platform  │
                     │  accounts    │──starts──▶ px-server (one per session)
                     │  games       │◀─heartbeat, saved data─┘    ▲
                     │  SQLite      │                             │ UDP, with a
                     └──────────────┘          px-player ─────────┘ signed ticket
```

## Running a platform

```sh
cmake --build build/dev                  # builds px-platform, px-server, px-headless, ...
build/dev/services/platform/px-platform  # http://127.0.0.1:8730, data in ./platform-data
```

Open http://127.0.0.1:8730 in a browser for a list of the published games.
Make yourself an administrator (once you have an account):

```sh
px-platform --data platform-data --make-admin YourName
```

| Option | Default | Meaning |
|---|---|---|
| `--data DIR` | `platform-data` | database, packages, game server logs and the platform's signing key |
| `--port N` | 8730 | HTTP port |
| `--bind ADDRESS` | 127.0.0.1 | listen address (`0.0.0.0` for other computers) |
| `--public-host HOST` | 127.0.0.1 | the address players are told to reach game servers at |
| `--platform-url URL` | from the port | how game servers reach the platform |
| `--server-ports FIRST:COUNT` | 27960:100 | UDP ports for game servers |
| `--max-servers N` | 20 | game servers at once |
| `--headless FILE`, `--server FILE` | next to px-platform, else the build's | the programs it runs |
| `--https-port N`, `--tls-cert FILE`, `--tls-key FILE` | off | HTTPS on a second port ([below](#https)) |
| `--https-bind ADDRESS` | `--bind` | listen address for HTTPS |
| `--web-player DIR` | `web-player/` next to px-platform | the WebAssembly player: play in browsers at `/play` ([below](#browsers)) |
| `--no-rate-limits` | | for testing |

To let other computers on your network play, start it with
`--bind 0.0.0.0 --public-host <this computer's address>` and allow the HTTP
(or HTTPS) port and the server ports, UDP and TCP, through the firewall.
Read [Security](#security) first.

### HTTPS

```sh
px-platform --data platform-data --dev-certificate localhost,127.0.0.1   # a self-signed certificate, for testing
px-platform --https-port 8731 --tls-cert platform-data/tls/dev-cert.pem --tls-key platform-data/tls/dev-key.pem
```

The same API is then served on both ports; game servers on the platform's
computer keep using the plain HTTP port. Game servers get the certificate
too, so browsers on an HTTPS page join them over secure WebSockets
(`wss://`). For other computers, use a certificate for the platform's public
name (for example from Let's Encrypt; `--tls-cert` takes the full chain).

Clients (Studio, px-player, px-headless) accept `https://` platform
addresses and check certificates against the system's store (Windows
certificate store, macOS keychain, Linux CA bundles). To trust a development
certificate, set `PX_PLATFORM_CA=/path/to/dev-cert.pem`; without it they
refuse the connection with a message saying so. Built with Mbed TLS
([ADR 0025](adr/0025-tls-with-mbed-tls.md)); `-DPX_ENABLE_TLS=OFF` builds
without it.

## Accounts

Create an account in Studio (*Platform > Publish, Play and Log In...*) or
with `px-player`/`px-headless --register`. Names are 3 to 20 letters, digits
or `_`, unique regardless of case; passwords need at least 8 characters.
Logging in gives a session token that lasts 30 days. Studio and the player
remember it in your user folder (`login.json`, readable only by you where
the system has file permissions), so logging in once in Studio is enough for
the player too.

## Publishing

In Studio, open the *Platform* panel's **Publish** tab, give the game a
title, description and visibility, and press *Create Game and Publish*.
Studio saves the project, packs it into a [game package](#game-packages)
and uploads it. Later, *Publish New Version* uploads the current state as
the next version (the link to the game is kept in the project's `.px/`
folder). From the command line:

```sh
px-headless path/to/project --platform http://127.0.0.1:8730 --user Ada --password '...' --publish --title "Coin Run"
px-headless path/to/project --platform ... --publish --game 1 --notes "faster coins"   # a new version
```

The platform **checks every upload before accepting it**, in a separate
`px-headless --check-package` process with a time and memory limit: the
manifest, every scene, prefab, animation, tileset, script and visual script
must load, every image is decoded and **re-encoded as PNG**, and the start
scene is simulated for two seconds. A game that fails is refused with the
reason (for example `scripts/coin.luau:3: ...`). Runtime script errors seen
in those two seconds are reported to the creator but do not block
publishing.

| Visibility | Who can see and play it |
|---|---|
| Public | everyone; listed in the catalog |
| Unlisted | anyone who knows its id; not listed |
| Private | only the owner (and administrators) |

The **My Games** tab changes title, description, visibility and players per
server, and shows a player's saved data in your game (for debugging).

## Playing

The **Play** tab lists the public games (most played, playing now or
recently updated; search by title or description). *Play* opens the game in
px-player. From the command line:

```sh
px-player --platform http://127.0.0.1:8730 --play 1            # as the account logged in from Studio
px-player --platform ... --play 1 --user Bob --password '...'  # or log in here
```

The player asks the platform to join, downloads the game version the
server runs (kept by its hash in your user folder, so each version is
downloaded once) and connects to the game server with the ticket it got.
Every game runs on a server, even for one player: that is what makes saved
data trustworthy.

### Browsers

With `--web-player DIR` (the `px-player.js`, `.wasm` and `.data` files of
the WebAssembly build, `cmake --preset wasm`), the platform serves a page at
**`/play`**: log in or make an account, pick a game, play. Browser players
join the **same game servers** as desktop players, over WebSockets
([ADR 0024](adr/0024-websockets-for-browsers.md)); keyboard, gamepads and
touch screens work ([input-devices.md](input-devices.md)).

```sh
px-platform --web-player build/wasm/apps/player       # then open http://127.0.0.1:8730/play
```

The page asks for a join ticket, downloads the game package while the
player loads, and starts the player with the ticket's `ws://` (or `wss://`)
address and token. `/play#game=ID` opens a game directly (the home page
links there). The page and its scripts are built into px-platform
(`services/platform/web`); the player files are revalidated with an ETag, so
browsers download them again only when they change.

Tested in CI: headless Chromium plays the top-down example through this
page while a desktop bot joins the same server over UDP; both see two
players and the level completed, over HTTP and over HTTPS with `wss://`
(`tools/browser/crossplay.sh`).

## Game servers

When someone joins a game, the platform gives them a seat on a running
server of the game's latest version that has room, or starts a new
`px-server` process for it (on the next free port), and waits until it
reports that it is listening. A server listens on its UDP port for desktop
players and on the TCP port with the same number for browsers (WebSockets;
`wss://` when the platform has a certificate). A server:

- only lets in players with a valid **connect ticket** (below);
- loads each player's saved data before they join, saves changes every
  30 seconds, when they leave and when it stops;
- tells the platform every 5 seconds who is playing;
- stops by itself 30 seconds after the last player left (60 seconds if
  nobody ever came), or when the platform asks (moderation);
- writes its log to `<data>/logs/server-<session>.log`.

A player is in **one game at a time**: while a server holds their saved
data, joining another game is refused until they have left (their data is
saved first). A seat that nobody takes is freed after a minute.

### Joining a game

A ticket is a **connect token** signed by the platform's private key
([ADR 0022](adr/0022-connect-tokens-and-game-sessions.md)): it names the
session, the account and its name, and expires after 60 seconds. Game
servers check it with the platform's public key: the signature, the
session, the expiry, and that it was not used before. The name players see
comes from the token, so nobody can play as someone else. Servers started
without `--platform` (LAN games, Studio's multiplayer test) accept anyone,
as before.

## Saved player data

Scripts (and visual scripts) keep values for each player between visits
([ADR 0023](adr/0023-saved-player-data.md)):

```lua
function Coin:TriggerEnter(other)
    if not other:hasComponent("Player") then return end
    Data.increment(other, "coins", self.value)   -- lifetime coins, kept by the platform
end

function Rules:PlayerJoined(player)
    local best = Data.get(player, "bestTime")    -- nil the first time
    Data.set(player, "lastVisit", "garden")
end
```

| Function | |
|---|---|
| `Data.get(player, key)` | the value or nil |
| `Data.set(player, key, value)` | a number, boolean, string or vector; nil removes it |
| `Data.increment(player, key, amount?)` | adds (default 1) to a number, missing counts as 0; returns the new value |

- `player` is a character (an entity with a Player component), as
  `PlayerJoined` and `TriggerEnter` give it. Data is per game and account.
- Keys are 1-50 letters, digits, `_`, `-` or `.`; text values up to 1000
  characters; at most 200 values and 64 KiB per player and game. Breaking a
  limit is a script error at the call that did it.
- The data is loaded **before** `PlayerJoined`, so it is always there. If
  the platform cannot provide it, the player is refused rather than started
  with empty data (nothing can be overwritten by mistake).
- Only authority scripts have `Data` (like `Game`).
- Played without a platform (Studio, a local server, `px-player <project>`)
  the data lives in memory: in Studio it is kept between plays until Studio
  closes, which is enough to test a game's saving.

The example platformer saves each player's lifetime coins.

## Moderation

Anyone logged in can **report** a game (*Report* in the Play tab). Players
see nothing of reports. Administrators get a **Moderation** tab:

- open reports, each with *Dismiss*, *Hide Game* or *Ban Owner*;
- hide or unhide any game by id: a hidden game disappears from the catalog,
  cannot be joined, and its running servers are asked to stop; its owner
  sees it marked hidden with the note;
- ban or unban an account: their sessions end, they cannot log in, and
  their games disappear (administrators cannot be banned);
- the **audit log**: every moderation action, who did it and when.

## Game packages

A published game is one file (`*.pxpkg`): `project.json` and every file its
assets refer to, nothing else (no `.px/` settings, tests or notes).
`px-headless <project> --pack game.pxpkg` writes one; px-headless,
px-server and px-player run packages like project folders.

Format (version 1, little-endian): `PXPK`, format version (u16), reserved
(u16), file count (u32); per file its path length (u16), path (normalized,
relative, sorted, unique) and size (u64); then the contents in the same
order. Decoders reject anything else (escaping paths, duplicates, sizes
that do not add up, more than 256 MiB). The platform names a package by its
BLAKE2b-256 hash, which clients check after downloading.

## HTTP API

JSON over HTTP under `/api/v1`. Errors are `{"error": {"code", "message"}}`
with a status code; messages are written for the person using the program.
`Bearer` is a player's session token, `Server` the key the platform gives a
game server when it starts it.

| Method and path | Auth | |
|---|---|---|
| `GET /platform` | | name, engine version, API version, public key, `webPlayer` (browser play available) |
| `POST /accounts` | | register `{username, password}` → `{user, token}` |
| `POST /sessions` | | log in → `{user, token}` |
| `DELETE /sessions/current` | Bearer | log out |
| `GET /me` | Bearer | the account |
| `GET /games?search=&sort=popular\|playing\|recent&limit=&offset=` | | the catalog |
| `GET /games/mine` | Bearer | your games |
| `POST /games` | Bearer | create `{title, description, visibility}` |
| `GET /games/{id}`, `PATCH /games/{id}` | (Bearer) | read; change (owner) |
| `POST /games/{id}/versions?notes=` | Bearer | publish: the package as the body |
| `GET /games/{id}/versions` | Bearer | versions (owner) |
| `GET /packages/{hash}` | (Bearer) | download a package of a game you may play |
| `POST /games/{id}/join` | Bearer | a ticket `{token, host, port, webSocket, session, expires, version, packageHash}` |
| `POST /games/{id}/reports` | Bearer | `{reason}` |
| `GET /games/{id}/data/{username}` | Bearer | a player's saved data (owner) |
| `GET /admin/reports?state=open\|all` | admin | reports |
| `POST /admin/reports/{id}/resolve` | admin | `{action: dismiss\|hide_game\|ban_owner, note}` |
| `POST /admin/games/{id}/hide` | admin | `{hidden, note}` |
| `POST /admin/users/{name}/ban` | admin | `{banned, note}` |
| `GET /admin/audit?limit=` | admin | the audit log |
| `POST /server/ready`, `POST /server/heartbeat` | Server | `{port}`; `{players}` → `{stop}` |
| `GET /server/data/{userId}`, `PUT /server/data/{userId}` | Server | load; save `{data, release}` |
| `POST /server/release/{userId}`, `POST /server/ended` | Server | a player left before loading; the server stops |

Outside `/api/v1`: `GET /` (a list of games), `GET /play` and
`/play/play.js`, `/play/play.css` (the browser page), `GET
/play/player/px-player.{js,wasm,data}` (the WebAssembly player, with `ETag`).

Rate limits (per address and account): 600 requests a minute, 20 log-ins a
minute (10 per name), 10 new accounts an hour, 20 new games and 30
publishes an hour, 30 joins a minute, 10 reports an hour. Over the limit:
status 429.

## Security

What protects what (see also [security.md](security.md)):

- **Passwords** are hashed with Argon2id (19 MiB, 2 passes, random salt);
  failed log-ins take the same time whether the name exists or not.
  **Session tokens** are 256-bit random values stored only as hashes.
- **Uploads** never run inside the service: they are checked by a separate
  process with a memory and time limit, images are re-encoded, and only the
  checked package is stored and served. Child processes get no open files
  or sockets of the service, only their own output file.
- **Game servers** run as separate processes with a memory limit; their key
  is passed in the environment, not on the command line. A server can only
  read and write the saved data of players the platform sent to it.
- **Tickets** are signed (EdDSA), short-lived, single-use and bound to one
  server session; a server needs no connection to the platform to check them.
- **The signing key** (`<data>/platform.key`) is readable only by the
  account running the platform. Anyone with it can let anyone join as
  anyone: back it up and keep it private.
- **HTTPS** ([above](#https)): with a certificate, passwords, session tokens
  and tickets travel encrypted, and browsers use `wss://` to game servers.
  Without one, run the platform on one computer or a trusted network only.
- **The browser page** sends a strict Content Security Policy (scripts only
  from the platform, WebAssembly allowed, no inline scripts, no framing),
  `X-Frame-Options: DENY` and `Referrer-Policy: no-referrer`; every answer
  has `X-Content-Type-Options: nosniff`. Text from games and accounts is
  inserted as text, never as HTML. The page never takes player arguments
  from its address (a link must not decide which server the player connects
  to with your ticket). The session token is kept in the tab's
  `sessionStorage`.

Not yet (planned):

- **UDP game traffic is not encrypted**, only authenticated at joining
  (browser players on `wss://` are encrypted).
- **Servers are processes, not containers:** they run as the platform's
  account with a memory limit, not in an isolated container with CPU and
  network limits.
- No email, password reset, two-factor authentication or account deletion;
  no profanity filter on titles; one platform process (no clustering).

## Command-line tools

| Command | |
|---|---|
| `px-platform ...` | run the platform (above) |
| `px-headless <project> --pack FILE.pxpkg` | write a game package |
| `px-headless --check-package FILE [--output FILE] [--ticks N]` | the platform's upload check (prints a JSON report) |
| `px-headless <project> --platform URL --user NAME --password TEXT [--register] --publish [--game ID] [--title T] [--notes N]` | publish |
| `px-headless --platform URL --user ... --play ID [--input-script FILE] [--expect-level-complete]` | join as a scripted bot |
| `px-player --platform URL --play ID [--user ... --password ... --register]` | play |
| `px-player --package FILE.pxpkg --connect ws://HOST:PORT/play --token TEXT` | what the browser page runs (any player can) |
| `tools/browser/crossplay.sh BUILD WEB_PLAYER OUT [http\|https]` | the browser cross-play test |
| `px-server FILE.pxpkg --platform URL --session ID --platform-key HEX` | how the platform starts servers (`PX_SERVER_KEY` in the environment) |

`PX_PLATFORM_DIR` overrides where programs keep the login and the package
cache (tests use it).

## Limitations

- One process with one SQLite database: fine for a small community, not for
  millions of players. Game servers run on the same computer as the
  platform.
- No player launcher in the desktop and mobile apps yet: browsing happens in
  Studio's Platform panel and on the browser page (`/play`).
- No profiles, avatars, friends or invitations; no thumbnails; no
  leaderboards or data shared between players (only per-player data).
- Players cannot pick a server or play with friends on purpose: seats are
  filled in order.
- Saved data is kept per game; there is no versioning, export or deletion
  tool for it yet.
