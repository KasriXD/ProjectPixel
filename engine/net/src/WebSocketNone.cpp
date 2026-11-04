// Builds without cpp-httplib (a desktop build with the platform switched off).
#include "px/net/WebSocket.hpp"

namespace px::net {

bool webSocketServerSupported() noexcept { return false; }
bool webSocketClientSupported() noexcept { return false; }

Result<std::unique_ptr<INetEndpoint>> listenWebSocket(const WebSocketServerOptions&) {
    return makeError(ErrorCode::Unsupported, "This build has no WebSocket support");
}

Result<std::unique_ptr<INetEndpoint>> connectWebSocket(const std::string&) {
    return makeError(ErrorCode::Unsupported, "This build has no WebSocket support");
}

} // namespace px::net
