#pragma once

#include "px/core/Result.hpp"
#include "px/net/Transport.hpp"

#include <cstdint>
#include <memory>

// An in-process network for tests and for simulating real conditions. Time
// is set by the owner (setTime), so runs are exactly repeatable; latency,
// jitter, loss and duplication are drawn from a seeded generator.
namespace px::net {

struct LinkConditions {
    double latencyMs = 0.0;        // one-way delay (the round trip is twice this)
    double jitterMs = 0.0;         // +/- random variation of unreliable messages
    double lossPercent = 0.0;      // unreliable messages dropped
    double duplicatePercent = 0.0; // unreliable messages delivered twice
};

namespace detail {
struct LoopbackHub;
}

class LoopbackNetwork {
public:
    explicit LoopbackNetwork(std::uint64_t seed = 1);
    ~LoopbackNetwork();
    LoopbackNetwork(const LoopbackNetwork&) = delete;
    LoopbackNetwork& operator=(const LoopbackNetwork&) = delete;

    // Current time in seconds; messages are delivered when it passes their due time.
    void setTime(double seconds);
    [[nodiscard]] double time() const;

    // A server endpoint on `port`; fails if the port is taken.
    [[nodiscard]] Result<std::unique_ptr<INetEndpoint>> listen(std::uint16_t port, std::size_t maxPeers = 64);
    // A client endpoint connecting to `port`. It reports Connected (or
    // Disconnected, when nobody listens or the server is full) after one round trip.
    [[nodiscard]] std::unique_ptr<INetEndpoint> connect(std::uint16_t port, const LinkConditions& conditions = {});

private:
    std::shared_ptr<detail::LoopbackHub> m_hub;
};

} // namespace px::net
