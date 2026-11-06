# Friends and profiles

What players see of each other on a platform: profiles, friends, presence,
joining friends, invitations, blocking and reports. Players use them in the
launcher and the browser page ([launcher.md](launcher.md)); programs use
the HTTP API below. Design: [ADR 0027](adr/0027-friends-presence-and-blocking.md).

**Status:** implemented in the platform (schema version 2), the launcher and
the browser page; covered by unit tests with real game servers (presence,
joining a friend, invitations, blocked players kept apart) and by the
launcher and browser checks in CI.

## Profiles

Every account has a profile: one of 16 pixel portraits (the same picture in
every app, `/play/avatars.png`), up to 200 characters about yourself, and
who may join you in games:

- **Friends** (the default): friends can join the game server you are on.
- **Only friends I invite**: friends join only with your invitation.

Anyone logged in can see a profile: name, portrait, text, when the account
was made, the number of friends and the account's public games. Presence is
shown only to friends.

## Friends

Send a friend request by name; when they accept, you are friends. If they
had already asked you, the request makes you friends at once. You can take
a request back, decline one, and remove a friend at any time (for both of
you). Limits: 200 friends, 100 requests waiting for an answer, 30 requests
an hour.

### Presence

Friends see each other as:

- **Playing** a game, with its title, when the game is one they may see
  (private games of others show as just *Playing*), and whether they may
  join you now.
- **Online**: the account used the platform in the last two minutes (the
  launcher and the browser page ask every 10 to 15 seconds while open).
- **Offline** otherwise.

### Joining a friend

*Join* asks the platform for a ticket to the game server your friend is on.
It works when you are friends, they are playing a game you may play, the
server has room, and their setting lets you (friends, or you hold their
invitation). The game is downloaded as for *Play*.

### Invitations

While you play a platform game, you can invite friends to your game server
(the game's menu in the apps, the panel beside the game in the browser). An
invitation lasts ten minutes, or until you leave that server, and lets that
friend join you even when you take only invited friends. Up to 30
invitations every ten minutes.

## Blocking

Blocking someone ends your friendship, deletes requests and invitations
between you, stops them from sending you new ones, and keeps you apart in
games: the platform never seats the two of you on the same game server
(when every server of a game has someone you blocked, or who blocked you, a
new one starts). They are not told. Unblock on your page. Up to 1000 blocks,
60 an hour.

## Reports

Report an account (name, portrait or text, or behaviour in games) with a
short reason: 10 reports an hour. Administrators see them
(`GET /admin/user-reports`) and resolve each with:

- `dismiss`: nothing to do;
- `clear_profile`: the text about them is emptied and the portrait reset;
- `ban`: the account is banned (it is logged out everywhere and its games'
  servers stop).

Every resolution is in the audit log.

## HTTP API

Under `/api/v1`, with a player's `Bearer` token
([platform.md](platform.md#http-api) for the conventions).

| Method and path | |
|---|---|
| `GET /users?search=PREFIX` | up to 20 accounts whose names start with PREFIX (2 letters or more) |
| `GET /users/{name}` | a profile `{user, bio, avatar, relation, games, friends, presence?, joinPolicy?}`; `relation`: `self`, `friend`, `requested`, `incoming`, `blocked`, `none` |
| `PATCH /me` | `{bio?, avatar?, joinPolicy?: friends\|nobody}` → your profile (30 an hour) |
| `GET /me/social` | `{friends, incoming, outgoing, invitations}` in one answer |
| `POST /friends/requests` | `{username}` → `{status: requested\|friends}` |
| `POST /friends/requests/{name}/accept` | accept their request |
| `DELETE /friends/requests/{name}` | decline theirs or take back yours |
| `DELETE /friends/{name}` | no longer friends |
| `POST /friends/{name}/join` | a ticket to their game server |
| `POST /friends/{name}/invite` | invite them to the server you are on |
| `DELETE /invitations/{id}` | dismiss an invitation |
| `GET /blocks`, `POST /blocks`, `DELETE /blocks/{name}` | the people you blocked; block `{username}`; unblock |
| `POST /users/{name}/reports` | `{reason}` |
| `GET /admin/user-reports?state=open\|all` | admin: reports about accounts |
| `POST /admin/user-reports/{id}/resolve` | admin: `{action: dismiss\|clear_profile\|ban, note}` |

C++ programs use `online::PlatformClient` (`profile`, `updateProfile`,
`searchUsers`, `social`, `requestFriend`, `acceptFriend`, ..., `invite`).

## Privacy

- Only friends see whether you are online or playing, and what.
- Names are public, as before; the text about you is visible to anyone
  logged in, so it is limited and can be reported and cleared.
- The platform keeps when each account last used it (for presence) and
  nothing about where from.

## Limitations

- No messages or chat between players (planned with moderation tools for
  text).
- No notifications outside the apps and the page.
- A friend in a game you may not see (private) cannot be joined.
- Blocking keeps you apart only on game servers the platform starts; a
  player-run server (`px-server`, `--host`) does not know who blocked whom.
