#include "px/net/WebSocket.hpp"

#include "px/net/Udp.hpp"

#include <map>
#include <utility>

namespace px::net {

namespace {

class CombinedEndpoint final : public INetEndpoint {
public:
    explicit CombinedEndpoint(std::vector<std::unique_ptr<INetEndpoint>> parts) : m_parts(std::move(parts)) {}

    void poll(std::vector<NetEvent>& out) override {
        for (std::size_t i = 0; i < m_parts.size(); ++i) {
            m_scratch.clear();
            m_parts[i]->poll(m_scratch);
            for (NetEvent& ev : m_scratch) {
                const Key key{i, ev.peer};
                PeerId outer = kNoPeer;
                if (ev.type == NetEvent::Type::Connected) {
                    outer = m_next++;
                    if (m_next == kNoPeer) m_next = 1;
                    m_outer[key] = outer;
                    m_inner[outer] = key;
                } else {
                    const auto it = m_outer.find(key);
                    if (it == m_outer.end()) continue;
                    outer = it->second;
                    if (ev.type == NetEvent::Type::Disconnected) forget(outer);
                }
                ev.peer = outer;
                out.push_back(std::move(ev));
            }
        }
    }

    bool send(PeerId peer, Delivery delivery, std::span<const std::uint8_t> data) override {
        const auto it = m_inner.find(peer);
        return it != m_inner.end() && m_parts[it->second.first]->send(it->second.second, delivery, data);
    }

    void flush() override {
        for (auto& p : m_parts) p->flush();
    }

    void disconnect(PeerId peer, std::uint32_t reason) override {
        const auto it = m_inner.find(peer);
        if (it == m_inner.end()) return;
        m_parts[it->second.first]->disconnect(it->second.second, reason);
        forget(peer);
    }

    [[nodiscard]] PeerStats stats(PeerId peer) const override {
        const auto it = m_inner.find(peer);
        return it == m_inner.end() ? PeerStats{} : m_parts[it->second.first]->stats(it->second.second);
    }

    [[nodiscard]] std::string describe(PeerId peer) const override {
        const auto it = m_inner.find(peer);
        return it == m_inner.end() ? std::string("?") : m_parts[it->second.first]->describe(it->second.second);
    }

private:
    using Key = std::pair<std::size_t, PeerId>;
    void forget(PeerId outer) {
        const auto it = m_inner.find(outer);
        if (it == m_inner.end()) return;
        m_outer.erase(it->second);
        m_inner.erase(it);
    }

    std::vector<std::unique_ptr<INetEndpoint>> m_parts;
    std::vector<NetEvent> m_scratch;
    std::map<Key, PeerId> m_outer;
    std::map<PeerId, Key> m_inner;
    PeerId m_next = 1;
};

} // namespace

Result<std::unique_ptr<INetEndpoint>> connectToServer(const std::string& address, std::uint16_t defaultPort) {
    if (address.starts_with("ws://") || address.starts_with("wss://")) return connectWebSocket(address);
    auto hostPort = parseAddress(address, defaultPort);
    if (!hostPort) return hostPort.error();
    return connectUdp(hostPort->first, hostPort->second);
}

std::unique_ptr<INetEndpoint> combineEndpoints(std::vector<std::unique_ptr<INetEndpoint>> endpoints) {
    if (endpoints.size() == 1) return std::move(endpoints.front());
    return std::make_unique<CombinedEndpoint>(std::move(endpoints));
}

} // namespace px::net
