# ADR 0029: The launcher lives in px-player, drawn with Dear ImGui

- Status: Accepted
- Date: 2026-11-05

## Context

Milestone 8 asks for a launcher in the desktop, mobile and browser apps:
log in, browse games, see friends and leaderboards, play. Until now
px-player played one game given on the command line (or the game in a phone
app), Studio was the only place to browse a platform, and the browser had a
small `/play` page. ADR 0013 kept Dear ImGui out of the games; the player
had no user interface toolkit besides the engine's debug text.

## Decision

1. **One program**: px-player opens the launcher when started without a
   game (or with `--launcher`). The launcher and the game it starts share
   one window: `SdlPlatform` can be reconfigured for a game and handed
   back, and `PlayerApp` can be created with an existing window and give it
   back when the game ends. `main.cpp` passes the window between them.
2. **Dear ImGui draws the launcher**, with the SDL3 renderer backend, as in
   Studio. Games still never use ImGui: it is linked into px-player only
   for the launcher, and only when the platform client is built.
3. **Requests run on worker threads** (one `std::async` per request with a
   copy of the client); results are applied on the UI thread, and results
   from an earlier login are dropped.
4. **The browser keeps its own page**: the platform's `/play` page grows
   the same pages in HTML (the WebAssembly player has no launcher), so the
   browser does not download a second UI toolkit.
5. **Phones** get the launcher too: the Android and iOS builds now include
   the platform client. The game built into the app is listed under *On
   this device*. Android uses the system's certificate folder for HTTPS;
   iOS has no API for its trusted roots usable from Mbed TLS, so the app
   takes a certificate file at build time (`PX_CA_BUNDLE`).
6. **Inviting from a game**: the game's menu gets *Invite a friend* (a list
   drawn with the engine's debug text, like the rest of the menu) when the
   player is in a platform game with a login.
7. **Tests drive the real UI**: `--open PAGE` and `--frames N` (counting
   only frames when nothing is loading) give deterministic screenshots of
   each page and of the round trip launcher → game → launcher.

## Alternatives considered

- **A separate launcher program** starting px-player: two windows (or a
  window closing and opening) and process plumbing on phones, where one app
  is one process.
- **A launcher written with the engine's own UI** (sprites and debug text):
  no new dependency in the player, but text input, scrolling and layout
  would all have to be written; ImGui is already vendored and tested.
- **A web view** (the `/play` page in every app): one UI everywhere, but a
  browser engine in the desktop app, and native web views differ per
  system.
- **Only the browser page**: no work in the apps, but phones and computers
  would need a browser to find games.

## Consequences

- px-player grows by ImGui (about 1 MB) on desktop and phones; the
  WebAssembly player does not.
- The launcher looks like a tool rather than a game store; theming and a
  font with more scripts are later work.
- Two launchers to keep in step (ImGui and HTML) against one API.
- ADR 0013 stays true for games; the player is no longer ImGui-free.
