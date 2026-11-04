// WebSocket endpoints on cpp-httplib (desktop and server builds). Each
// connection has a reader thread (httplib's handler) and a sender thread;
// the game thread only touches queues, so a slow client never stalls a tick.
#include "px/net/WebSocket.hpp"

#include "px/core/Log.hpp"

#include <httplib.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <thread>

namespace px::net {

namespace {

constexpr std::uint8_t kFrameData = 0;
constexpr std::uint8_t kFrameDisconnect = 1;
constexpr const char* kPath = "/play";

std::string dataFrame(std::span<const std::uint8_t> data) {
    std::string frame(1 + data.size(), '\0');
    frame[0] = static_cast<char>(kFrameData);
    if (!data.empty()) std::memcpy(frame.data() + 1, data.data(), data.size());
    return frame;
}

std::string disconnectFrame(std::uint32_t reason) {
    std::string frame(5, '\0');
    frame[0] = static_cast<char>(kFrameDisconnect);
    for (int i = 0; i < 4; ++i) frame[1 + i] = static_cast<char>((reason >> (8 * i)) & 0xFF);
    return frame;
}

std::uint32_t frameReason(const std::string& frame) {
    std::uint32_t r = 0;
    for (int i = 0; i < 4 && 1 + static_cast<std::size_t>(i) < frame.size(); ++i) {
        r |= std::uint32_t{static_cast<std::uint8_t>(frame[1 + static_cast<std::size_t>(i)])} << (8 * i);
    }
    return r;
}

// ---------------------------------------------------------------------------------------------

class WsServerEndpoint final : public INetEndpoint {
public:
    explicit WsServerEndpoint(WebSocketServerOptions options) : m_options(std::move(options)) {}

    ~WsServerEndpoint() override {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            for (auto& [id, p] : m_peers) {
                p->closing = true;
                p->wake.notify_all();
            }
        }
        if (m_server) m_server->stop();
        if (m_thread.joinable()) m_thread.join();
    }

    Status start() {
#if defined(PX_HAVE_TLS)
        if (!m_options.tlsCert.empty()) {
            auto ssl = std::make_unique<httplib::SSLServer>(m_options.tlsCert.c_str(), m_options.tlsKey.c_str());
            if (!ssl->is_valid()) {
                return makeError(ErrorCode::InvalidData, "Cannot use the TLS certificate {}", m_options.tlsCert);
            }
            m_server = std::move(ssl);
        }
#else
        if (!m_options.tlsCert.empty()) return makeError(ErrorCode::Unsupported, "This build has no TLS support");
#endif
        if (!m_server) m_server = std::make_unique<httplib::Server>();
        const std::size_t threads = m_options.maxPeers + 4; // a connection holds a thread while it is open
        m_server->new_task_queue = [threads] { return new httplib::ThreadPool(threads); };
        m_server->set_tcp_nodelay(true); // snapshots go out at once
        m_server->WebSocket(kPath, [this](const httplib::Request& req, httplib::ws::WebSocket& ws) { serve(req, ws); });
        if (!m_server->bind_to_port(m_options.bindAddress, m_options.port)) {
            return makeError(ErrorCode::IoError, "Cannot listen for WebSockets on TCP port {} (is it in use?)",
                             m_options.port);
        }
        m_thread = std::thread([this] { m_server->listen_after_bind(); });
        m_server->wait_until_ready();
        return {};
    }

    void poll(std::vector<NetEvent>& out) override {
        std::lock_guard lock(m_mutex);
        for (NetEvent& ev : m_events) out.push_back(std::move(ev));
        m_events.clear();
    }

    bool send(PeerId peer, Delivery, std::span<const std::uint8_t> data) override {
        std::lock_guard lock(m_mutex);
        const auto it = m_peers.find(peer);
        if (it == m_peers.end() || it->second->closing) return false;
        Peer& p = *it->second;
        p.outgoing.push_back(dataFrame(data));
        p.stats.bytesSent += data.size();
        p.wake.notify_one();
        return true;
    }

    void flush() override {}

    void disconnect(PeerId peer, std::uint32_t reason) override {
        std::lock_guard lock(m_mutex);
        const auto it = m_peers.find(peer);
        if (it == m_peers.end()) return;
        Peer& p = *it->second;
        p.outgoing.push_back(disconnectFrame(reason));
        p.closing = true;     // the sender closes after this frame
        p.closedHere = true;  // no Disconnected event for our own disconnect
        p.wake.notify_one();
    }

    [[nodiscard]] PeerStats stats(PeerId peer) const override {
        std::lock_guard lock(m_mutex);
        const auto it = m_peers.find(peer);
        return it == m_peers.end() ? PeerStats{} : it->second->stats;
    }

    [[nodiscard]] std::string describe(PeerId peer) const override {
        std::lock_guard lock(m_mutex);
        const auto it = m_peers.find(peer);
        return it == m_peers.end() ? std::string("?") : it->second->address;
    }

private:
    struct Peer {
        std::deque<std::string> outgoing;
        std::condition_variable wake;
        bool closing = false;
        bool closedHere = false;
        std::string address;
        PeerStats stats;
    };

    // Runs on httplib's thread for one connection, until it closes.
    void serve(const httplib::Request& req, httplib::ws::WebSocket& ws) {
        PeerId id = kNoPeer;
        std::shared_ptr<Peer> peer;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping || m_peers.size() >= m_options.maxPeers) {
                const std::string refused = disconnectFrame(kReasonRefused);
                (void)ws.send(refused.data(), refused.size()); // binary: the string overload sends text
                ws.close();
                return;
            }
            id = m_next++;
            if (m_next == kNoPeer) m_next = 1;
            peer = std::make_shared<Peer>();
            peer->address = std::format("ws {}:{}", req.remote_addr, req.remote_port);
            m_peers[id] = peer;
            m_events.push_back({NetEvent::Type::Connected, id, {}, 0});
        }
        std::thread sender([this, peer, &ws] {
            std::unique_lock lock(m_mutex);
            while (true) {
                peer->wake.wait(lock, [&] { return !peer->outgoing.empty() || peer->closing; });
                while (!peer->outgoing.empty()) {
                    std::string frame = std::move(peer->outgoing.front());
                    peer->outgoing.pop_front();
                    lock.unlock();
                    const bool sent = ws.send(frame.data(), frame.size());
                    lock.lock();
                    if (!sent) peer->closing = true;
                }
                if (peer->closing) break;
            }
            lock.unlock();
            ws.close();
        });
        std::string msg;
        std::uint32_t reason = kReasonLost;
        bool remoteSaidGoodbye = false;
        ws.set_read_timeout(1, 0); // to notice when the server stops
        while (true) {
            const auto r = ws.read(msg);
            if (r == httplib::ws::Timeout) {
                std::lock_guard lock(m_mutex);
                if (m_stopping || peer->closing) break;
                continue;
            }
            if (r != httplib::ws::Binary || msg.empty()) break; // closed, or text (not ours)
            if (static_cast<std::uint8_t>(msg[0]) == kFrameDisconnect) {
                reason = frameReason(msg);
                remoteSaidGoodbye = true;
                break;
            }
            std::lock_guard lock(m_mutex);
            peer->stats.bytesReceived += msg.size() - 1;
            m_events.push_back({NetEvent::Type::Received, id,
                                std::vector<std::uint8_t>(msg.begin() + 1, msg.end()), 0});
        }
        {
            std::lock_guard lock(m_mutex);
            if (!peer->closedHere && !m_stopping) {
                m_events.push_back({NetEvent::Type::Disconnected, id, {}, remoteSaidGoodbye ? reason : kReasonLost});
            }
            peer->closing = true;
            peer->wake.notify_all();
        }
        sender.join();
        std::lock_guard lock(m_mutex);
        m_peers.erase(id);
    }

    WebSocketServerOptions m_options;
    std::unique_ptr<httplib::Server> m_server;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    std::map<PeerId, std::shared_ptr<Peer>> m_peers;
    std::vector<NetEvent> m_events;
    PeerId m_next = 1;
    bool m_stopping = false;
};

// ---------------------------------------------------------------------------------------------

class WsClientEndpoint final : public INetEndpoint {
public:
    explicit WsClientEndpoint(std::string url) : m_url(std::move(url)) {
        m_thread = std::thread([this] { run(); });
    }

    ~WsClientEndpoint() override {
        m_stopping = true;
        {
            std::lock_guard lock(m_mutex);
            if (m_ws) m_ws->close();
        }
        if (m_thread.joinable()) m_thread.join();
    }

    void poll(std::vector<NetEvent>& out) override {
        std::lock_guard lock(m_mutex);
        for (NetEvent& ev : m_events) out.push_back(std::move(ev));
        m_events.clear();
    }

    bool send(PeerId peer, Delivery, std::span<const std::uint8_t> data) override {
        if (peer != kServer) return false;
        std::lock_guard lock(m_mutex);
        if (!m_ws || !m_open) return false;
        const std::string frame = dataFrame(data);
        m_stats.bytesSent += data.size();
        return m_ws->send(frame.data(), frame.size());
    }

    void flush() override {}

    void disconnect(PeerId peer, std::uint32_t reason) override {
        if (peer != kServer) return;
        std::lock_guard lock(m_mutex);
        m_closedHere = true;
        if (m_ws && m_open) {
            const std::string frame = disconnectFrame(reason);
            (void)m_ws->send(frame.data(), frame.size());
            m_ws->close();
        }
        m_open = false;
    }

    [[nodiscard]] PeerStats stats(PeerId) const override {
        std::lock_guard lock(m_mutex);
        return m_stats;
    }
    [[nodiscard]] std::string describe(PeerId) const override { return m_url; }

private:
    static constexpr PeerId kServer = 1;

    void run() {
        std::unique_ptr<httplib::ws::WebSocketClient> ws;
        try {
            ws = std::make_unique<httplib::ws::WebSocketClient>(m_url);
        } catch (const std::exception&) {
        }
        if (ws) {
            ws->set_connection_timeout(10, 0);
            ws->set_tcp_nodelay(true); // inputs go out at once, not batched
            ws->set_read_timeout(1, 0); // to notice when we are destroyed
        }
        if (!ws || !ws->is_valid() || !ws->connect()) {
            std::lock_guard lock(m_mutex);
            m_events.push_back({NetEvent::Type::Disconnected, kServer, {}, kReasonRefused});
            return;
        }
        {
            std::lock_guard lock(m_mutex);
            m_ws = ws.get();
            m_open = !m_stopping;
            m_events.push_back({NetEvent::Type::Connected, kServer, {}, 0});
        }
        std::string msg;
        std::uint32_t reason = kReasonLost;
        while (!m_stopping) {
            const auto r = ws->read(msg);
            if (r == httplib::ws::Timeout) continue;
            if (r != httplib::ws::Binary || msg.empty()) break;
            if (static_cast<std::uint8_t>(msg[0]) == kFrameDisconnect) {
                reason = frameReason(msg);
                break;
            }
            std::lock_guard lock(m_mutex);
            m_stats.bytesReceived += msg.size() - 1;
            m_events.push_back({NetEvent::Type::Received, kServer,
                                std::vector<std::uint8_t>(msg.begin() + 1, msg.end()), 0});
        }
        std::lock_guard lock(m_mutex);
        if (!m_closedHere && !m_stopping) m_events.push_back({NetEvent::Type::Disconnected, kServer, {}, reason});
        m_open = false;
        ws->close();
        m_ws = nullptr;
    }

    std::string m_url;
    std::thread m_thread;
    mutable std::mutex m_mutex;
    httplib::ws::WebSocketClient* m_ws = nullptr;
    std::vector<NetEvent> m_events;
    PeerStats m_stats;
    bool m_open = false;
    bool m_closedHere = false;
    std::atomic<bool> m_stopping{false};
};

} // namespace

bool webSocketServerSupported() noexcept { return true; }
bool webSocketClientSupported() noexcept { return true; }

Result<std::unique_ptr<INetEndpoint>> listenWebSocket(const WebSocketServerOptions& options) {
    auto endpoint = std::make_unique<WsServerEndpoint>(options);
    if (auto started = endpoint->start(); !started) return started.error();
    return std::unique_ptr<INetEndpoint>(std::move(endpoint));
}

Result<std::unique_ptr<INetEndpoint>> connectWebSocket(const std::string& url) {
    if (!url.starts_with("ws://") && !url.starts_with("wss://")) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is not a ws:// or wss:// address", url);
    }
#if !defined(PX_HAVE_TLS)
    if (url.starts_with("wss://")) return makeError(ErrorCode::Unsupported, "This build has no TLS support");
#endif
    return std::unique_ptr<INetEndpoint>(std::make_unique<WsClientEndpoint>(url));
}

} // namespace px::net
