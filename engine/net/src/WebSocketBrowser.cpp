// The WebSocket client in browsers (Emscripten builds): the browser's own
// WebSocket, through Emscripten's API. Callbacks run on the main thread
// between frames and queue events for poll().
#include "px/net/WebSocket.hpp"

#include <emscripten/websocket.h>

#include <cstring>

namespace px::net {

namespace {

constexpr std::uint8_t kFrameData = 0;
constexpr std::uint8_t kFrameDisconnect = 1;
constexpr PeerId kServer = 1;

class BrowserWsEndpoint final : public INetEndpoint {
public:
    explicit BrowserWsEndpoint(std::string url) : m_url(std::move(url)) {
        EmscriptenWebSocketCreateAttributes attrs;
        emscripten_websocket_init_create_attributes(&attrs);
        attrs.url = m_url.c_str();
        attrs.protocols = nullptr;
        attrs.createOnMainThread = EM_TRUE;
        m_socket = emscripten_websocket_new(&attrs);
        if (m_socket <= 0) {
            m_events.push_back({NetEvent::Type::Disconnected, kServer, {}, kReasonRefused});
            m_socket = 0;
            return;
        }
        emscripten_websocket_set_onopen_callback(m_socket, this, onOpen);
        emscripten_websocket_set_onmessage_callback(m_socket, this, onMessage);
        emscripten_websocket_set_onerror_callback(m_socket, this, onError);
        emscripten_websocket_set_onclose_callback(m_socket, this, onClose);
    }

    ~BrowserWsEndpoint() override {
        if (m_socket > 0) {
            emscripten_websocket_set_onopen_callback(m_socket, nullptr, nullptr);
            emscripten_websocket_set_onmessage_callback(m_socket, nullptr, nullptr);
            emscripten_websocket_set_onerror_callback(m_socket, nullptr, nullptr);
            emscripten_websocket_set_onclose_callback(m_socket, nullptr, nullptr);
            emscripten_websocket_close(m_socket, 1000, "");
            emscripten_websocket_delete(m_socket);
        }
    }

    void poll(std::vector<NetEvent>& out) override {
        for (NetEvent& ev : m_events) out.push_back(std::move(ev));
        m_events.clear();
    }

    bool send(PeerId peer, Delivery, std::span<const std::uint8_t> data) override {
        if (peer != kServer || !m_open) return false;
        m_buffer.assign(1, kFrameData);
        m_buffer.insert(m_buffer.end(), data.begin(), data.end());
        m_stats.bytesSent += data.size();
        return emscripten_websocket_send_binary(m_socket, m_buffer.data(), static_cast<std::uint32_t>(m_buffer.size())) ==
               EMSCRIPTEN_RESULT_SUCCESS;
    }

    void flush() override {}

    void disconnect(PeerId peer, std::uint32_t reason) override {
        if (peer != kServer || m_socket <= 0) return;
        if (m_open) {
            std::uint8_t frame[5] = {kFrameDisconnect, static_cast<std::uint8_t>(reason & 0xFF),
                                     static_cast<std::uint8_t>((reason >> 8) & 0xFF),
                                     static_cast<std::uint8_t>((reason >> 16) & 0xFF),
                                     static_cast<std::uint8_t>((reason >> 24) & 0xFF)};
            emscripten_websocket_send_binary(m_socket, frame, sizeof frame);
        }
        m_open = false;
        m_closedHere = true;
        emscripten_websocket_close(m_socket, 1000, "");
    }

    [[nodiscard]] PeerStats stats(PeerId) const override { return m_stats; }
    [[nodiscard]] std::string describe(PeerId) const override { return m_url; }

private:
    static EM_BOOL onOpen(int, const EmscriptenWebSocketOpenEvent*, void* user) {
        auto* self = static_cast<BrowserWsEndpoint*>(user);
        self->m_open = true;
        self->m_events.push_back({NetEvent::Type::Connected, kServer, {}, 0});
        return EM_TRUE;
    }
    static EM_BOOL onMessage(int, const EmscriptenWebSocketMessageEvent* e, void* user) {
        auto* self = static_cast<BrowserWsEndpoint*>(user);
        if (e->isText || e->numBytes == 0) return EM_TRUE;
        if (e->data[0] == kFrameDisconnect && e->numBytes >= 5) {
            std::uint32_t reason = 0;
            for (int i = 0; i < 4; ++i) reason |= std::uint32_t{e->data[1 + i]} << (8 * i);
            self->m_reason = reason;
            self->m_gotReason = true;
            return EM_TRUE;
        }
        self->m_stats.bytesReceived += e->numBytes - 1;
        self->m_events.push_back({NetEvent::Type::Received, kServer,
                                  std::vector<std::uint8_t>(e->data + 1, e->data + e->numBytes), 0});
        return EM_TRUE;
    }
    static EM_BOOL onError(int, const EmscriptenWebSocketErrorEvent*, void*) { return EM_TRUE; }
    static EM_BOOL onClose(int, const EmscriptenWebSocketCloseEvent*, void* user) {
        auto* self = static_cast<BrowserWsEndpoint*>(user);
        const bool wasOpen = self->m_open;
        self->m_open = false;
        if (!self->m_closedHere) {
            const std::uint32_t reason = self->m_gotReason ? self->m_reason : (wasOpen ? kReasonLost : kReasonRefused);
            self->m_events.push_back({NetEvent::Type::Disconnected, kServer, {}, reason});
        }
        self->m_closedHere = true;
        return EM_TRUE;
    }

    std::string m_url;
    EMSCRIPTEN_WEBSOCKET_T m_socket = 0;
    std::vector<NetEvent> m_events;
    std::vector<std::uint8_t> m_buffer;
    PeerStats m_stats;
    bool m_open = false;
    bool m_closedHere = false;
    bool m_gotReason = false;
    std::uint32_t m_reason = 0;
};

} // namespace

bool webSocketServerSupported() noexcept { return false; }
bool webSocketClientSupported() noexcept { return true; }

Result<std::unique_ptr<INetEndpoint>> listenWebSocket(const WebSocketServerOptions&) {
    return makeError(ErrorCode::Unsupported, "Browsers cannot host games");
}

Result<std::unique_ptr<INetEndpoint>> connectWebSocket(const std::string& url) {
    if (!url.starts_with("ws://") && !url.starts_with("wss://")) {
        return makeError(ErrorCode::InvalidArgument, "'{}' is not a ws:// or wss:// address", url);
    }
    return std::unique_ptr<INetEndpoint>(std::make_unique<BrowserWsEndpoint>(url));
}

} // namespace px::net
