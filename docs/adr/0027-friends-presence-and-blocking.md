# ADR 0027: Friends, presence and blocking in the platform

- Status: Accepted
- Date: 2026-11-05

## Context

Milestone 8 lets players find each other: profiles, friends, seeing who is
online or playing, joining a friend's game, inviting friends, and keeping
away from people. The platform already knows accounts (ADR 0021) and which
game server each player has a seat on (the fleet, ADR 0022). Players can be
children; whatever is social must be limited, reportable and private by
default.

## Decision

1. **In the platform service**, in the same SQLite database (schema
   version 2: `friends`, `friend_requests`, `blocks`, `invitations`,
   `user_reports`, and profile columns on `users`), with handlers in their
   own file (`Social.cpp`) sharing the service's context.
2. **Friends are mutual**: a request and an acceptance (two rows, one per
   direction, for simple queries); a request to someone who already asked
   you accepts theirs. Limits: 200 friends, 100 open requests.
3. **Presence comes from what the platform already sees**: the time of each
   account's last request (`last_seen`, online for 120 s) and the fleet's
   seats (playing, which game, which server). No separate connection and
   no heartbeat from games. Presence is shown only to friends.
4. **Joining a friend** asks the fleet for a seat on the friend's server
   (`reserveIn`); the ticket is the usual signed connect token. A per-account
   setting decides whether friends may join freely or only with an
   **invitation**, which is tied to the inviter's server and lasts 10
   minutes.
5. **Blocking** removes the friendship, requests and invitations both ways,
   and the fleet never seats two people with a block between them on one
   server (an *avoid* set when choosing a server; a new server when none
   fits). The blocked person is not told.
6. **Reports about accounts** go to administrators, who dismiss them, clear
   the profile (text and portrait) or ban; every action is audited.
7. **No free text between players** in this milestone: the only text is the
   200-character "about you", which anyone can report. Portraits are 16
   built-in pictures, not uploads.

## Alternatives considered

- **A separate social service** (or an off-the-shelf one): more moving
  parts, and joining friends needs the fleet's seats anyway.
- **Presence over a long-lived connection** (WebSocket from every client):
  instant updates, but a connection per player to run and secure; polling
  every 10-15 s is enough to see who plays and costs one small request.
- **One-way following**: simpler, but lets anyone watch anyone's presence.
- **Uploaded avatars**: personal, but every picture would need moderation;
  later, with moderation tools.
- **Chat**: the most asked-for social feature and the riskiest for young
  players; it needs filtering and moderation first.

## Consequences

- Presence is up to 2 minutes late for "online" and immediate for
  "playing" (seats change at once).
- Blocking can start more game servers than strictly needed.
- Player-run servers outside the platform know nothing of friends or blocks.
- Later: chat with filtering, notifications (push, e-mail), parental
  settings, uploaded portraits with moderation.
