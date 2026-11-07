# The launcher

How players who only play find games, friends and scores: the launcher in
px-player (computers, Android, iOS) and the platform's `/play` page
(browsers). Both talk to a platform ([platform.md](platform.md)); friends
and invitations are described in [social.md](social.md), leaderboards in
[leaderboards.md](leaderboards.md). Design: [ADR 0029](adr/0029-launcher-in-the-player.md).

**Status:** implemented. The desktop launcher is tested against a real
platform on Linux in CI (every page, playing, joining a friend, inviting
from a game, the saved login) and was used with a mouse and keyboard on
Linux (X11). The browser page is tested in headless Chromium with the real
WebAssembly player in CI, at computer and phone sizes. The phone apps open
the launcher on an emulated Android phone and a simulated iPhone in CI.
**Not yet used by a person on a phone or in a browser.**

## px-player

Started without a game, px-player opens the launcher:

```sh
px-player                                   # the launcher
px-player --platform https://example.com   # the launcher, with this platform's address filled in
```

- **Log in** with the platform's address, your name and password, or
  *Create an account*. *Stay logged in on this device* keeps the login (the
  same login Studio uses; logging in from either logs in both). The next
  start logs in by itself. *Forgot your password?* sends a reset link to
  the account's confirmed email address ([accounts.md](accounts.md)).
- **Games**: the platform's games with their pictures, how often they were
  played and how many play right now; search and order (popular, new,
  playing now). A card opens the game's page.
- **A game's page**: picture, description, *Play*, friends playing it (with
  *Join* when they let you) and the game's leaderboards, for everyone or
  only your friends.
- **Friends**: invitations, requests to you (*Accept*, *Decline*), your
  friends with what they are doing (*Join* when you may, *See the game*
  otherwise), requests you sent (*Take back*), and *Add a friend* by name
  (names starting with what you type are suggested). The *...* button
  removes a friend, blocks or reports someone.
- **You**: your portrait (one of 16), a short text about you, who may join
  you in games (friends, or only friends you invite), the people you
  blocked; your email address (confirmed by a link), changing your
  password, *Log out* and *Delete your account...* ([accounts.md](accounts.md)).
- **On this device**: games that play without a platform: the bundled
  examples on computers, the game that comes with the app on phones.

*Play* or *Join* starts the game in the same window. In the game's menu
(Esc, Start or the pause button) *Invite a friend* lists your friends;
inviting one shows them an invitation to the game server you are on, for
ten minutes or until you leave it. *Leave the game* (or *Back to
games* for games on this device) returns to the launcher; closing the
window quits.

Keyboard, gamepad and touch all work: the arrow keys (or the pad) move
between buttons, Space (or the pad's South button) presses one, and on
touch screens a finger drags the page to scroll. Android's back button goes back to the
games, then leaves the app.

### Options

| Option | |
|---|---|
| `--launcher` | open the launcher even with other options |
| `--platform URL` | the platform's address (without `--play`: for the launcher) |
| `--user NAME --password TEXT [--register]` | log in (or make the account) at once; with `--frames` the login is not saved |
| `--open PAGE` | the first page: `game:ID`, `friends`, `you`, `account` (your page, at its account settings); or start `play:ID`, `join:NAME` or `local:N` (the Nth game on this device) |
| `--frames N --screenshot FILE` | tests: draw N frames once nothing is loading, save the last one, quit; a game started with `--open` also plays N frames first, then returns to the launcher |
| `--invite NAME` | with `--play ID`: invite this friend once in the game |

`PX_PLATFORM_DIR` changes where the login and downloaded games are kept
(normally `~/.config/projectpixel/platform`, `%APPDATA%\ProjectPixel\platform`
or `~/Library/Application Support/ProjectPixel/platform`; on phones the
app's own folder). `PX_UI_SCALE` sets the size of the launcher's text and
buttons (normally from the screen).

### Phones and tablets

The Android and iOS apps open the launcher. The game built into the app is
listed under *On this device* and plays without a platform. On Android,
HTTPS platforms are checked against the phone's trusted certificates. iOS
gives the TLS library no access to the system's trusted certificates: build
the app with `-DPX_CA_BUNDLE=/path/to/roots.pem` (for example Mozilla's
list, as curl ships it) to log in to HTTPS platforms; `http://` platforms
work without it (only on networks you trust).

Test runs on Android pass options as an extra:
`adb shell am start -n dev.px.player/.PlayerActivity --es args "--open local:0"`.
Other apps can start the app the same way, so only `--open` with pages or
games on the device, `--frames`, `--verbose`, `--touch` and `--launcher`
are taken from it.

## The browser

The platform's `/play` page does the same in a browser: the games with
pictures (*Play* on each card plays at once), a page per game with its
leaderboards, friends, your profile and account settings (email address,
password, downloading your data, deleting the account), and while playing,
a panel to invite friends to the game server you are on. Playing needs the
WebAssembly player on the platform (`--web-player`, see
[platform.md](platform.md#browsers)); without it the page has everything
but the *Play* buttons and says to use the app. Links in the platform's
email open `#verify=TOKEN` (confirms the address) and `#reset=TOKEN`
(choose a new password) on this page. Addresses: `/play#games`, `#game=ID`, `#friends`, `#you`;
`#play=ID` and `#join=NAME` start the player only when the page itself went
there after a click, so a link from elsewhere cannot make you join a game
or a person: it opens the game's page or the friends page.

The player cannot be stopped inside a page: leaving a game, or opening
another page while playing, loads the page again.

## Game pictures

Games show a picture in the catalog: 320x180 pixels, made by the platform
from any PNG or JPEG of at least 16x9 pixels (up to 4 MiB), in a separate
process like game uploads. Owners send one with Studio's *Picture of the
open scene* when publishing (what the game's camera shows), with
`px-headless --publish --thumbnail PICTURE`, or with
`PUT /api/v1/games/{id}/thumbnail`. Pictures are served with an `ETag`.

## Testing

| Test | What it covers |
|---|---|
| `launcher_smoke_headless_display` (ctest, every platform) | the launcher opens without a platform, starts a game on this device, gets the window back and draws again |
| `tools/launcher/launcher_check.sh BUILD_DIR OUT_DIR` (Linux CI) | a platform with a published game, three accounts and a bot playing: every launcher page with screenshots, playing from the launcher, joining a friend, inviting a friend from a game, the saved login |
| `tools/browser/social_test.sh NATIVE WEB OUT [desktop\|phone]` (CI, with the real WebAssembly player) | the browser page: pictures, a game's page and leaderboards, accepting and sending friend requests, the profile, links not starting the player, joining a friend and inviting one; nothing wider than the screen |
| `tools/browser/accounts_test.sh NATIVE OUT [files\|smtp] [desktop\|phone]` (CI) | the page without a browser player: account settings, links from the mail, password reset and change, the data download, deleting the account |
| `tools/android/emulator_check.sh`, the iOS job | the apps open the launcher, start the game on the device and go back to the launcher |

## Limitations

- The launcher shows the platform's text in Dear ImGui's built-in font:
  Latin letters only (names are Latin by rule; game titles and
  descriptions in other scripts show as `?`).
- No notifications outside the app: invitations show when the launcher,
  the page or the game's menu next asks the platform (every 10 to 15
  seconds).
- One platform at a time; switching means logging out.
- The browser page cannot stop a running game without loading again.
