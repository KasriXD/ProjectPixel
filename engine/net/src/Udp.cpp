#include "px/net/Udp.hpp"

#include <charconv>
#include <format>

#if PX_HAVE_ENET
#include "px/core/Log.hpp"

#include <enet/enet.h>

#include <cstdlib>
#include <map>
#include <mutex>
#endif

namespace px::net {

Result<std::pair<std::string, std::uint16_t>> parseAddress(std::string_view text, std::uint16_t defaultPort) {
    std::string_view host = text;
    std::uint16_t port = defaultPort;
    if (const auto colon = text.rfind(':'); colon != std::string_view::npos) {
        host = text.substr(0, colon);
        const std::string_view portText = text.substr(colon + 1);
        unsigned value = 0;
        const auto [ptr, ec] = std::from_chars(portText.data(), portText.data() + portText.size(), value);
        if (ec != std::errc() || ptr != portText.data() + portText.size() || value < 1 || value > 65535) {
            return makeError(ErrorCode::InvalidArgument, "'{}' is not a valid port (1-65535)", portText);
        }
        port = static_cast<std::uint16_t>(value);
    }
    if (host.empty()) return makeError(ErrorCode::InvalidArgument, "No server address in '{}'", text);
    return std::pair<std::string, std::uint16_t>{std::string(host), port};
}

#if PX_HAVE_ENET

namespace {

constexpr std::size_t kChannelReliable = 0;
constexpr std::size_t kChannelUnreliable = 1;
constexpr std::size_t kChannels = 2;
constexpr std::size_t kMaxPacketBytes = 8 * 1024 * 1024;

bool initializeEnet() {
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        ok = enet_initialize() == 0;
        if (ok) std::atexit(enet_deinitialize);
    });
    return ok;
}

std::string addressText(const ENetAddress& address) {
    char ip[64] = {};
    if (enet_address_get_host_ip(&address, ip, sizeof ip) != 0) return "?";
    return std::format("{}:{}", ip, address.port);
}

class UdpEndpoint final : public INetEndpoint {
public:
    UdpEndpoint(ENetHost* host, bool server) : m_host(host), m_server(server) {
        m_host->maximumPacketSize = kMaxPacketBytes;
    }

    ~UdpEndpoint() override {
        for (const auto& [id, peer] : m_peers) enet_peer_disconnect_now(peer.peer, kReasonLost);
        enet_host_flush(m_host);
        enet_host_destroy(m_host);
    }

    void setConnecting(ENetPeer* peer) { m_connecting = peer; }

    void poll(std::vector<NetEvent>& out) override {
        ENetEvent event;
        while (enet_host_service(m_host, &event, 0) > 0) {
            switch (event.type) {
                case ENET_EVENT_TYPE_CONNECT: {
                    const PeerId id = m_server ? m_nextPeer++ : 1;
                    event.peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
                    // Notice a vanished peer within seconds rather than ENet's default 30.
                    enet_peer_timeout(event.peer, 0, 3000, 10000);
                    m_peers[id] = Peer{event.peer, {}};
                    m_connecting = nullptr;
                    out.push_back({NetEvent::Type::Connected, id, {}, 0});
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE: {
                    const PeerId id = idOf(event.peer);
                    if (id != kNoPeer) {
                        std::vector<std::uint8_t> data(event.packet->data, event.packet->data + event.packet->dataLength);
                        m_peers[id].stats.bytesReceived += data.size();
                        out.push_back({NetEvent::Type::Received, id, std::move(data), 0});
                    }
                    enet_packet_destroy(event.packet);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT: {
                    if (event.peer == m_connecting) {
                        // The connection attempt failed: nobody answered.
                        m_connecting = nullptr;
                        out.push_back({NetEvent::Type::Disconnected, 1, {}, kReasonRefused});
                        break;
                    }
                    const PeerId id = idOf(event.peer);
                    event.peer->data = nullptr;
                    if (id == kNoPeer) break; // we closed it ourselves
                    m_peers.erase(id);
                    out.push_back({NetEvent::Type::Disconnected, id, {}, event.data});
                    break;
                }
                case ENET_EVENT_TYPE_NONE: break;
            }
        }
    }

    bool send(PeerId id, Delivery delivery, std::span<const std::uint8_t> data) override {
        const auto it = m_peers.find(id);
        if (it == m_peers.end() || data.size() > kMaxPacketBytes) return false;
        const bool reliable = delivery == Delivery::Reliable;
        // Large unreliable messages are split into unreliable fragments
        // (otherwise ENet would quietly make them reliable).
        ENetPacket* packet = enet_packet_create(data.data(), data.size(),
                                                reliable ? ENET_PACKET_FLAG_RELIABLE
                                                         : ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT);
        if (packet == nullptr) return false;
        if (enet_peer_send(it->second.peer, static_cast<enet_uint8>(reliable ? kChannelReliable : kChannelUnreliable),
                           packet) != 0) {
            enet_packet_destroy(packet);
            return false;
        }
        it->second.stats.bytesSent += data.size();
        return true;
    }

    void flush() override { enet_host_flush(m_host); }

    void disconnect(PeerId id, std::uint32_t reason) override {
        const auto it = m_peers.find(id);
        if (it == m_peers.end()) return;
        it->second.peer->data = nullptr; // no Disconnected event for our own decision
        // After everything queued (a Reject message) has been delivered.
        enet_peer_disconnect_later(it->second.peer, reason);
        m_peers.erase(it);
    }

    PeerStats stats(PeerId id) const override {
        const auto it = m_peers.find(id);
        if (it == m_peers.end()) return {};
        PeerStats s = it->second.stats;
        s.roundTripMs = it->second.peer->roundTripTime;
        s.packetLoss = static_cast<double>(it->second.peer->packetLoss) / static_cast<double>(ENET_PEER_PACKET_LOSS_SCALE);
        return s;
    }

    std::string describe(PeerId id) const override {
        const auto it = m_peers.find(id);
        return it == m_peers.end() ? std::string("?") : addressText(it->second.peer->address);
    }

private:
    struct Peer {
        ENetPeer* peer = nullptr;
        PeerStats stats;
    };

    static PeerId idOf(const ENetPeer* peer) {
        return static_cast<PeerId>(reinterpret_cast<std::uintptr_t>(peer->data));
    }

    ENetHost* m_host;
    bool m_server;
    PeerId m_nextPeer = 1;
    std::map<PeerId, Peer> m_peers;
    ENetPeer* m_connecting = nullptr;
};

} // namespace

bool udpSupported() { return true; }

Result<std::unique_ptr<INetEndpoint>> listenUdp(std::uint16_t port, std::size_t maxPeers) {
    if (!initializeEnet()) return makeError(ErrorCode::Unsupported, "Could not start networking (ENet)");
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;
    // A few extra slots so connections beyond the limit can be told the server is full.
    ENetHost* host = enet_host_create(&address, maxPeers + 4, kChannels, 0, 0);
    if (host == nullptr) {
        return makeError(ErrorCode::IoError, "Could not listen on UDP port {} (is another server using it?)", port);
    }
    return std::unique_ptr<INetEndpoint>(std::make_unique<UdpEndpoint>(host, true));
}

Result<std::unique_ptr<INetEndpoint>> connectUdp(const std::string& hostName, std::uint16_t port) {
    if (!initializeEnet()) return makeError(ErrorCode::Unsupported, "Could not start networking (ENet)");
    ENetAddress address{};
    if (enet_address_set_host(&address, hostName.c_str()) != 0) {
        return makeError(ErrorCode::NotFound, "Unknown server address '{}'", hostName);
    }
    address.port = port;
    ENetHost* host = enet_host_create(nullptr, 1, kChannels, 0, 0);
    if (host == nullptr) return makeError(ErrorCode::IoError, "Could not open a UDP socket");
    auto endpoint = std::make_unique<UdpEndpoint>(host, false);
    ENetPeer* peer = enet_host_connect(host, &address, kChannels, 0);
    if (peer == nullptr) return makeError(ErrorCode::IoError, "Could not start connecting to {}:{}", hostName, port);
    peer->data = nullptr;
    endpoint->setConnecting(peer);
    log::debug("Connecting to {}", addressText(address));
    return std::unique_ptr<INetEndpoint>(std::move(endpoint));
}

#else

bool udpSupported() { return false; }

Result<std::unique_ptr<INetEndpoint>> listenUdp(std::uint16_t, std::size_t) {
    return makeError(ErrorCode::Unsupported, "This build has no UDP networking");
}

Result<std::unique_ptr<INetEndpoint>> connectUdp(const std::string&, std::uint16_t) {
    return makeError(ErrorCode::Unsupported, "This build has no UDP networking");
}

#endif

} // namespace px::net
