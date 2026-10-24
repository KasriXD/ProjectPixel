#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// The transport moves whole messages between a server and its clients. The
// game protocol on top of it only needs two kinds of delivery, so it can run
// over UDP (ENet) today and over WebSockets/WebTransport for browsers later
// (docs/adr/0018-multiplayer-networking.md).
namespace px::net {

// A connection, as seen from one endpoint. 0 is never a valid peer.
using PeerId = std::uint32_t;
inline constexpr PeerId kNoPeer = 0;

enum class Delivery : std::uint8_t {
    Reliable,   // arrives once, in order with other reliable messages (handshake)
    Unreliable, // may be lost, duplicated or reordered (inputs, snapshots)
};

// Disconnect reasons reported by transports themselves (the game protocol's
// own reasons are small numbers; see Protocol.hpp).
inline constexpr std::uint32_t kReasonLost = 0;            // timed out or closed without a reason
inline constexpr std::uint32_t kReasonRefused = 0xFFFF0001; // nobody listening, or no room

struct NetEvent {
    enum class Type : std::uint8_t { Connected, Disconnected, Received };
    Type type = Type::Received;
    PeerId peer = kNoPeer;
    std::vector<std::uint8_t> data; // Received only
    std::uint32_t reason = 0;       // Disconnected: code from the other side, 0 = lost or closed
};

struct PeerStats {
    double roundTripMs = 0.0;
    double packetLoss = 0.0; // 0..1, as estimated by the transport
    std::uint64_t bytesSent = 0;
    std::uint64_t bytesReceived = 0;
};

// One side of the transport: a listening server with many peers, or a client
// with one (the server). Non-blocking; poll() returns everything that
// arrived since the last call. Endpoints are not thread-safe.
class INetEndpoint {
public:
    virtual ~INetEndpoint() = default;

    virtual void poll(std::vector<NetEvent>& out) = 0;
    // Queues a message. Returns false if the peer is unknown or the message
    // cannot be sent (too large).
    virtual bool send(PeerId peer, Delivery delivery, std::span<const std::uint8_t> data) = 0;
    // Pushes queued messages onto the wire (call once per frame or tick).
    virtual void flush() = 0;
    // Closes the connection; the other side sees Disconnected with `reason`.
    // No Disconnected event is reported for it locally.
    virtual void disconnect(PeerId peer, std::uint32_t reason) = 0;

    [[nodiscard]] virtual PeerStats stats(PeerId peer) const = 0;
    // "127.0.0.1:7777", for logs.
    [[nodiscard]] virtual std::string describe(PeerId peer) const = 0;
};

} // namespace px::net
