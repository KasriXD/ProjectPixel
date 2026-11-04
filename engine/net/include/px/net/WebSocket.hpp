#pragma once

#include "px/core/Result.hpp"
#include "px/net/Transport.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// WebSockets: the transport browsers can use (they cannot send UDP).
// Servers listen for WebSockets on a TCP port next to their UDP port, and
// GameServer serves both through one combined endpoint, so browser and
// desktop players are in the same game (docs/multiplayer.md#browsers,
// ADR 0024). Over TCP every message is delivered in order; the game
// protocol's "unreliable" messages simply arrive.
//
// Each WebSocket message is one transport message: a type byte (0: data,
// 1: disconnect, followed by the 32-bit reason) and the payload.
namespace px::net {

// Whether this build can listen for WebSockets (desktop builds with cpp-httplib).
[[nodiscard]] bool webSocketServerSupported() noexcept;
// Whether this build can connect over WebSockets (desktop and browser builds).
[[nodiscard]] bool webSocketClientSupported() noexcept;

struct WebSocketServerOptions {
    std::uint16_t port = 0;           // TCP port
    std::size_t maxPeers = 64;
    std::string bindAddress = "0.0.0.0";
    // wss:// (TLS): PEM certificate and key. Pages served over https:// can
    // only open secure WebSockets.
    std::string tlsCert;
    std::string tlsKey;
};

// Listens for WebSocket connections on path "/play".
[[nodiscard]] Result<std::unique_ptr<INetEndpoint>> listenWebSocket(const WebSocketServerOptions& options);
// Connects to "ws://host:port/play" or "wss://...". The connection is made
// in the background: Connected (or Disconnected) arrives through poll().
[[nodiscard]] Result<std::unique_ptr<INetEndpoint>> connectWebSocket(const std::string& url);

// Connects to a game server: "ws://..." or "wss://..." over WebSockets,
// anything else ("host" or "host:port") over UDP.
[[nodiscard]] Result<std::unique_ptr<INetEndpoint>> connectToServer(const std::string& address,
                                                                    std::uint16_t defaultPort);

// Several listening endpoints (UDP and WebSocket) as one; peer ids stay unique.
[[nodiscard]] std::unique_ptr<INetEndpoint> combineEndpoints(std::vector<std::unique_ptr<INetEndpoint>> endpoints);

} // namespace px::net
