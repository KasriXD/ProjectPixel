#pragma once

#include "px/core/Result.hpp"
#include "px/net/Transport.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// The UDP transport (ENet): what px-server and px-player use on desktops.
// Reliable messages go on one channel, unreliable ones on another; large
// unreliable messages are fragmented without being made reliable. Browser
// builds do not have it (browsers cannot send UDP; see ADR 0018).
namespace px::net {

[[nodiscard]] bool udpSupported();

// A server endpoint on UDP `port`, on every network interface.
[[nodiscard]] Result<std::unique_ptr<INetEndpoint>> listenUdp(std::uint16_t port, std::size_t maxPeers);
// A client endpoint connecting to `host` (a name or an IPv4 address) on `port`.
// Name lookup happens here and may take a moment.
[[nodiscard]] Result<std::unique_ptr<INetEndpoint>> connectUdp(const std::string& host, std::uint16_t port);

// "host" or "host:port" (port 1-65535); the port defaults to `defaultPort`.
[[nodiscard]] Result<std::pair<std::string, std::uint16_t>> parseAddress(std::string_view text,
                                                                         std::uint16_t defaultPort);

} // namespace px::net
