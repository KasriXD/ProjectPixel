#include "px/net/Loopback.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <unordered_map>

namespace px::net {

namespace detail {

namespace {

enum class Kind : std::uint8_t { ConnectRequest, Accept, Refuse, Data, Close };

struct Packet {
    double deliverAt = 0.0;
    std::uint64_t order = 0; // ties keep send order
    std::uint32_t to = 0;    // endpoint id
    std::uint32_t connection = 0;
    Kind kind = Kind::Data;
    std::vector<std::uint8_t> data;
    std::uint32_t reason = 0;
};

struct Connection {
    std::uint32_t client = 0; // endpoint ids
    std::uint32_t server = 0;
    LinkConditions conditions;
    PeerId serverPeer = kNoPeer; // the client's peer id on the server
    double lastReliable[2] = {0.0, 0.0}; // [0] towards the server, [1] towards the client
    bool open = true;
    PeerStats stats[2]; // [0] the client's view, [1] the server's view
};

// splitmix64: the same numbers on every platform (std distributions differ).
struct Random {
    std::uint64_t state;
    double next() {
        std::uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
    }
};

} // namespace

struct LoopbackHub {
    double now = 0.0;
    Random random{1};
    std::uint64_t order = 0;
    std::uint32_t nextEndpoint = 1;
    std::uint32_t nextConnection = 1;
    std::map<std::uint16_t, std::uint32_t> listeners; // port -> server endpoint
    std::unordered_map<std::uint32_t, std::size_t> serverCapacity;
    std::unordered_map<std::uint32_t, std::size_t> serverPeers;
    std::vector<Packet> inFlight;
    std::unordered_map<std::uint32_t, Connection> connections;

    void post(Packet p) {
        p.order = ++order;
        inFlight.push_back(std::move(p));
    }

    // Packets for `endpoint` that are due, in delivery order.
    std::vector<Packet> take(std::uint32_t endpoint) {
        std::vector<Packet> due;
        std::erase_if(inFlight, [&](Packet& p) {
            if (p.to != endpoint || p.deliverAt > now) return false;
            due.push_back(std::move(p));
            return true;
        });
        std::sort(due.begin(), due.end(), [](const Packet& a, const Packet& b) {
            return a.deliverAt != b.deliverAt ? a.deliverAt < b.deliverAt : a.order < b.order;
        });
        return due;
    }

    // Due time of a message travelling over `c` in direction `dir` (0: to the server).
    double dueTime(Connection& c, int dir, Delivery delivery) {
        const LinkConditions& lc = c.conditions;
        double delay = lc.latencyMs / 1000.0;
        if (lc.jitterMs > 0.0) delay += (random.next() * 2.0 - 1.0) * lc.jitterMs / 1000.0;
        delay = std::max(0.0, delay);
        double at = now + delay;
        if (delivery == Delivery::Reliable) {
            at = std::max(at, c.lastReliable[dir]); // reliable messages stay in order
            c.lastReliable[dir] = at;
        }
        return at;
    }

    void close(std::uint32_t connectionId, int dir, std::uint32_t reason) {
        auto it = connections.find(connectionId);
        if (it == connections.end() || !it->second.open) return;
        Connection& c = it->second;
        c.open = false;
        if (c.serverPeer != kNoPeer) --serverPeers[c.server];
        Packet p;
        p.deliverAt = dueTime(c, dir, Delivery::Reliable);
        p.to = dir == 0 ? c.server : c.client;
        p.connection = connectionId;
        p.kind = Kind::Close;
        p.reason = reason;
        post(std::move(p));
    }
};

} // namespace detail

namespace {

using detail::Kind;
using detail::LoopbackHub;
using detail::Packet;

class LoopbackServer final : public INetEndpoint {
public:
    LoopbackServer(std::shared_ptr<LoopbackHub> hub, std::uint32_t id, std::uint16_t port)
        : m_hub(std::move(hub)), m_id(id), m_port(port) {}

    ~LoopbackServer() override {
        for (const auto& [peer, connection] : m_peers) m_hub->close(connection, 1, kReasonLost);
        m_hub->listeners.erase(m_port);
        // Clients still waiting to be let in are refused; anything else for us is dropped.
        std::vector<std::uint32_t> waiting;
        std::erase_if(m_hub->inFlight, [&](const Packet& p) {
            if (p.to != m_id) return false;
            if (p.kind == Kind::ConnectRequest) waiting.push_back(p.connection);
            return true;
        });
        for (const std::uint32_t connection : waiting) {
            auto it = m_hub->connections.find(connection);
            if (it == m_hub->connections.end()) continue;
            it->second.open = false;
            Packet refuse;
            refuse.to = it->second.client;
            refuse.connection = connection;
            refuse.kind = Kind::Refuse;
            refuse.reason = kReasonRefused;
            refuse.deliverAt = m_hub->dueTime(it->second, 1, Delivery::Reliable);
            m_hub->post(std::move(refuse));
        }
    }

    void poll(std::vector<NetEvent>& out) override {
        for (Packet& p : m_hub->take(m_id)) {
            auto it = m_hub->connections.find(p.connection);
            if (it == m_hub->connections.end()) continue;
            detail::Connection& c = it->second;
            switch (p.kind) {
                case Kind::ConnectRequest: {
                    Packet reply;
                    reply.to = c.client;
                    reply.connection = p.connection;
                    if (!c.open || m_hub->serverPeers[m_id] >= m_hub->serverCapacity[m_id]) {
                        c.open = false;
                        reply.kind = Kind::Refuse;
                        reply.reason = kReasonRefused;
                    } else {
                        const PeerId peer = m_nextPeer++;
                        c.serverPeer = peer;
                        ++m_hub->serverPeers[m_id];
                        m_peers[peer] = p.connection;
                        out.push_back({NetEvent::Type::Connected, peer, {}, 0});
                        reply.kind = Kind::Accept;
                    }
                    reply.deliverAt = m_hub->dueTime(c, 1, Delivery::Reliable);
                    m_hub->post(std::move(reply));
                    break;
                }
                case Kind::Data:
                    if (c.open && c.serverPeer != kNoPeer) {
                        c.stats[1].bytesReceived += p.data.size();
                        out.push_back({NetEvent::Type::Received, c.serverPeer, std::move(p.data), 0});
                    }
                    break;
                case Kind::Close:
                    if (c.serverPeer != kNoPeer && m_peers.erase(c.serverPeer) > 0) {
                        out.push_back({NetEvent::Type::Disconnected, c.serverPeer, {}, p.reason});
                    }
                    break;
                case Kind::Accept:
                case Kind::Refuse: break;
            }
        }
    }

    bool send(PeerId peer, Delivery delivery, std::span<const std::uint8_t> data) override {
        const auto it = m_peers.find(peer);
        if (it == m_peers.end()) return false;
        return sendOver(*m_hub, it->second, 1, delivery, data);
    }

    void flush() override {}

    void disconnect(PeerId peer, std::uint32_t reason) override {
        const auto it = m_peers.find(peer);
        if (it == m_peers.end()) return;
        m_hub->close(it->second, 1, reason);
        m_peers.erase(it);
    }

    PeerStats stats(PeerId peer) const override {
        const auto it = m_peers.find(peer);
        if (it == m_peers.end()) return {};
        const auto& c = m_hub->connections.at(it->second);
        PeerStats s = c.stats[1];
        s.roundTripMs = c.conditions.latencyMs * 2.0;
        s.packetLoss = c.conditions.lossPercent / 100.0;
        return s;
    }

    std::string describe(PeerId peer) const override { return std::format("loopback client {}", peer); }

    static bool sendOver(LoopbackHub& hub, std::uint32_t connection, int dir, Delivery delivery,
                         std::span<const std::uint8_t> data) {
        auto it = hub.connections.find(connection);
        if (it == hub.connections.end() || !it->second.open) return false;
        detail::Connection& c = it->second;
        c.stats[dir == 0 ? 0 : 1].bytesSent += data.size();
        int copies = 1;
        if (delivery == Delivery::Unreliable) {
            if (hub.random.next() * 100.0 < c.conditions.lossPercent) copies = 0;
            else if (hub.random.next() * 100.0 < c.conditions.duplicatePercent) copies = 2;
        }
        for (int i = 0; i < copies; ++i) {
            Packet p;
            p.deliverAt = hub.dueTime(c, dir, delivery);
            p.to = dir == 0 ? c.server : c.client;
            p.connection = connection;
            p.kind = Kind::Data;
            p.data.assign(data.begin(), data.end());
            hub.post(std::move(p));
        }
        return true;
    }

private:
    std::shared_ptr<LoopbackHub> m_hub;
    std::uint32_t m_id;
    std::uint16_t m_port;
    PeerId m_nextPeer = 1;
    std::map<PeerId, std::uint32_t> m_peers; // peer -> connection
};

class LoopbackClient final : public INetEndpoint {
public:
    static constexpr PeerId kServerPeer = 1;

    LoopbackClient(std::shared_ptr<LoopbackHub> hub, std::uint32_t id, std::uint32_t connection)
        : m_hub(std::move(hub)), m_id(id), m_connection(connection) {}

    ~LoopbackClient() override {
        // Also when the server has not answered yet: it must not keep a peer for us.
        m_hub->close(m_connection, 0, kReasonLost);
        std::erase_if(m_hub->inFlight, [this](const Packet& p) { return p.to == m_id; });
    }

    void poll(std::vector<NetEvent>& out) override {
        for (Packet& p : m_hub->take(m_id)) {
            switch (p.kind) {
                case Kind::Accept:
                    if (!m_closed) {
                        m_connected = true;
                        out.push_back({NetEvent::Type::Connected, kServerPeer, {}, 0});
                    }
                    break;
                case Kind::Refuse:
                    m_closed = true;
                    out.push_back({NetEvent::Type::Disconnected, kServerPeer, {}, p.reason});
                    break;
                case Kind::Data:
                    if (m_connected) {
                        m_hub->connections.at(m_connection).stats[0].bytesReceived += p.data.size();
                        out.push_back({NetEvent::Type::Received, kServerPeer, std::move(p.data), 0});
                    }
                    break;
                case Kind::Close:
                    if (m_connected) {
                        m_connected = false;
                        m_closed = true;
                        out.push_back({NetEvent::Type::Disconnected, kServerPeer, {}, p.reason});
                    }
                    break;
                case Kind::ConnectRequest: break;
            }
        }
    }

    bool send(PeerId peer, Delivery delivery, std::span<const std::uint8_t> data) override {
        if (peer != kServerPeer || !m_connected) return false;
        return LoopbackServer::sendOver(*m_hub, m_connection, 0, delivery, data);
    }

    void flush() override {}

    void disconnect(PeerId peer, std::uint32_t reason) override {
        if (peer != kServerPeer || !m_connected) return;
        m_connected = false;
        m_closed = true;
        m_hub->close(m_connection, 0, reason);
    }

    PeerStats stats(PeerId) const override {
        const auto& c = m_hub->connections.at(m_connection);
        PeerStats s = c.stats[0];
        s.roundTripMs = c.conditions.latencyMs * 2.0;
        s.packetLoss = c.conditions.lossPercent / 100.0;
        return s;
    }

    std::string describe(PeerId) const override { return "loopback server"; }

private:
    std::shared_ptr<LoopbackHub> m_hub;
    std::uint32_t m_id;
    std::uint32_t m_connection;
    bool m_connected = false;
    bool m_closed = false;
};

} // namespace

LoopbackNetwork::LoopbackNetwork(std::uint64_t seed) : m_hub(std::make_shared<LoopbackHub>()) {
    m_hub->random.state = seed;
}

LoopbackNetwork::~LoopbackNetwork() = default;

void LoopbackNetwork::setTime(double seconds) { m_hub->now = seconds; }
double LoopbackNetwork::time() const { return m_hub->now; }

Result<std::unique_ptr<INetEndpoint>> LoopbackNetwork::listen(std::uint16_t port, std::size_t maxPeers) {
    if (m_hub->listeners.contains(port)) {
        return makeError(ErrorCode::InvalidArgument, "Loopback port {} is already in use", port);
    }
    const std::uint32_t id = m_hub->nextEndpoint++;
    m_hub->listeners[port] = id;
    m_hub->serverCapacity[id] = maxPeers;
    m_hub->serverPeers[id] = 0;
    return std::unique_ptr<INetEndpoint>(std::make_unique<LoopbackServer>(m_hub, id, port));
}

std::unique_ptr<INetEndpoint> LoopbackNetwork::connect(std::uint16_t port, const LinkConditions& conditions) {
    const std::uint32_t id = m_hub->nextEndpoint++;
    const std::uint32_t connectionId = m_hub->nextConnection++;
    detail::Connection c;
    c.client = id;
    c.conditions = conditions;
    const auto listener = m_hub->listeners.find(port);
    Packet p;
    p.connection = connectionId;
    if (listener == m_hub->listeners.end()) {
        c.open = false;
        p.to = id;
        p.kind = Kind::Refuse;
        p.reason = kReasonRefused;
        p.deliverAt = m_hub->now + 2.0 * conditions.latencyMs / 1000.0;
    } else {
        c.server = listener->second;
        p.to = c.server;
        p.kind = Kind::ConnectRequest;
        p.deliverAt = m_hub->now + conditions.latencyMs / 1000.0;
        c.lastReliable[0] = p.deliverAt;
    }
    m_hub->connections.emplace(connectionId, c);
    m_hub->post(std::move(p));
    return std::make_unique<LoopbackClient>(m_hub, id, connectionId);
}

} // namespace px::net
