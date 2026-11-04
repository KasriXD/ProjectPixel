# ADR 0024: Browsers join game servers over WebSockets

- Status: Accepted
- Date: 2026-11-04

## Context

Browsers cannot send UDP, the transport of every game server (ADR 0018).
Milestone 7 lets browser players join the same game servers as desktop
players, so that a platform game has one community whatever device people
use. WebTransport (QUIC datagrams) would keep unreliable delivery but needs
HTTP/3 and certificates on every server, and Safari support arrived only
recently. WebSockets work in every browser, through most firewalls and
proxies, and cpp-httplib (already used by the platform) has a server and a
client.

## Decision

1. **Transport**: `net/WebSocket.hpp` implements the existing
   `INetEndpoint` interface over WebSockets. Each WebSocket binary message
   is one transport message: a type byte (0 data, 1 disconnect with a 32-bit
   reason) and the payload. Unreliable messages are simply delivered (TCP).
2. **Same servers**: `px-server` listens for WebSockets on the TCP port with
   the number of its UDP port (path `/play`), and `combineEndpoints` merges
   both into one endpoint for `GameServer`: desktop and browser players are
   in the same game, with the same protocol, tokens and checks.
3. **Native and browser clients**: desktop builds connect with cpp-httplib
   (a reader thread per connection), browser builds with Emscripten's
   WebSocket API. `connectToServer` picks WebSockets for `ws://` and
   `wss://` addresses and UDP otherwise.
4. **Tickets** carry a `webSocket` URL next to the UDP address. When the
   platform has a TLS certificate, game servers get it too and the URL is
   `wss://` (pages served over HTTPS may only open secure WebSockets).
5. **The browser page** is served by the platform itself (`/play`), with the
   WebAssembly player from `--web-player DIR`; it uses the same HTTP API as
   the desktop programs.

## Consequences

- One lost packet delays the following ones (head-of-line blocking): over a
  lossy network, browser players see more stutter than UDP players. The
  game protocol does not change, so WebTransport can replace WebSockets for
  browsers later without touching the game code.
- Each WebSocket connection uses two threads on the server (cpp-httplib);
  fine for the tens of players a game server holds.
- Game servers now listen on a TCP port as well; firewalls must allow it.
- Game traffic over `ws://` is not encrypted (as UDP); over `wss://` it is.
