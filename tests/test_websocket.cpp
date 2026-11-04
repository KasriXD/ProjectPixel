// WebSocket transport on real localhost sockets: messages both ways,
// disconnect reasons, refusing when full, and one game with a UDP player and
// a WebSocket (browser-like) player together.
#include <doctest.h>

#include "px/core/FileSystem.hpp"
#include "px/net/GameClient.hpp"
#include "px/net/GameServer.hpp"
#include "px/net/Udp.hpp"
#include "px/net/WebSocket.hpp"
#include "px/runtime/GameLoader.hpp"

#include <chrono>
#include <format>
#include <random>
#include <thread>

using namespace px;
using namespace px::net;

namespace {

std::uint16_t freePortGuess() {
    static std::uint16_t next = static_cast<std::uint16_t>(31000 + std::random_device{}() % 2000);
    return next++;
}

// Polls `endpoint` until `pred` holds for one of the events (max ~5 s).
template <class Pred>
bool waitFor(INetEndpoint& endpoint, std::vector<NetEvent>& seen, Pred pred) {
    for (int i = 0; i < 500; ++i) {
        std::vector<NetEvent> events;
        endpoint.poll(events);
        for (auto& e : events) {
            const bool hit = pred(e);
            seen.push_back(std::move(e));
            if (hit) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::vector<std::uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

} // namespace

TEST_CASE("WebSockets: messages, disconnect reasons and a full server") {
    if (!webSocketServerSupported()) return;
    WebSocketServerOptions options;
    options.port = freePortGuess();
    options.maxPeers = 1;
    auto server = listenWebSocket(options);
    REQUIRE_MESSAGE(server.ok(), (server.ok() ? "" : server.error().message));
    auto client = connectWebSocket(std::format("ws://127.0.0.1:{}/play", options.port));
    REQUIRE(client.ok());
    std::vector<NetEvent> serverSeen;
    std::vector<NetEvent> clientSeen;
    REQUIRE(waitFor(**client, clientSeen, [](const NetEvent& e) { return e.type == NetEvent::Type::Connected; }));
    REQUIRE(waitFor(**server, serverSeen, [](const NetEvent& e) { return e.type == NetEvent::Type::Connected; }));
    const PeerId peer = serverSeen.back().peer;
    CHECK((*server)->describe(peer).starts_with("ws 127.0.0.1:"));

    REQUIRE((*client)->send(1, Delivery::Unreliable, bytes("hello")));
    REQUIRE(waitFor(**server, serverSeen, [](const NetEvent& e) { return e.type == NetEvent::Type::Received; }));
    CHECK(serverSeen.back().data == bytes("hello"));
    std::vector<std::uint8_t> big(100000, 7);
    REQUIRE((*server)->send(peer, Delivery::Reliable, big));
    REQUIRE(waitFor(**client, clientSeen, [](const NetEvent& e) { return e.type == NetEvent::Type::Received; }));
    CHECK(clientSeen.back().data == big);

    // A second client is refused: the server is full.
    auto second = connectWebSocket(std::format("ws://127.0.0.1:{}/play", options.port));
    REQUIRE(second.ok());
    std::vector<NetEvent> secondSeen;
    REQUIRE(waitFor(**second, secondSeen, [](const NetEvent& e) { return e.type == NetEvent::Type::Disconnected; }));
    CHECK(secondSeen.back().reason == kReasonRefused);

    // The server ends the connection with a reason the client receives.
    (*server)->disconnect(peer, 5);
    REQUIRE(waitFor(**client, clientSeen, [](const NetEvent& e) { return e.type == NetEvent::Type::Disconnected; }));
    CHECK(clientSeen.back().reason == 5);
    CHECK_FALSE((*client)->send(1, Delivery::Reliable, bytes("late")));

    // Nobody listening: refused.
    auto nowhere = connectWebSocket(std::format("ws://127.0.0.1:{}/play", freePortGuess()));
    REQUIRE(nowhere.ok());
    std::vector<NetEvent> nowhereSeen;
    REQUIRE(waitFor(**nowhere, nowhereSeen, [](const NetEvent& e) { return e.type == NetEvent::Type::Disconnected; }));
    CHECK(nowhereSeen.back().reason == kReasonRefused);
    CHECK_FALSE(connectWebSocket("http://example.com").ok());
}

TEST_CASE("WebSockets: a UDP player and a WebSocket player in one game") {
    if (!webSocketServerSupported() || !udpSupported()) return;
    DirectoryFileSystem files(std::string(PX_EXAMPLES_DIR) + "/topdown");
    auto game = runtime::loadGame(files);
    REQUIRE(game.ok());
    const std::uint16_t port = freePortGuess();
    std::vector<std::unique_ptr<INetEndpoint>> endpoints;
    auto udp = listenUdp(port, 8);
    REQUIRE(udp.ok());
    endpoints.push_back(std::move(udp.value()));
    WebSocketServerOptions wsOptions;
    wsOptions.port = port;
    auto ws = listenWebSocket(wsOptions);
    REQUIRE(ws.ok());
    endpoints.push_back(std::move(ws.value()));
    auto server = GameServer::create(game.value(), combineEndpoints(std::move(endpoints)));
    REQUIRE(server.ok());

    std::vector<std::unique_ptr<GameClient>> clients;
    for (const std::string& address : {std::format("127.0.0.1:{}", port), std::format("ws://127.0.0.1:{}/play", port)}) {
        auto endpoint = connectToServer(address, kDefaultPort);
        REQUIRE(endpoint.ok());
        ClientOptions options;
        options.playerName = address.starts_with("ws") ? "Web" : "Desktop";
        auto c = GameClient::create(game.value(), std::move(endpoint.value()), options);
        REQUIRE(c.ok());
        clients.push_back(std::move(c.value()));
    }
    const auto start = std::chrono::steady_clock::now();
    std::uint32_t right = 0;
    for (std::size_t i = 0; i < game->manifest.input.actions.size(); ++i) {
        if (game->manifest.input.actions[i].name == "move_right") right = 1u << i;
    }
    for (int tick = 0; tick < 240; ++tick) {
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        (*server)->tick();
        for (auto& c : clients) {
            c->receive(now);
            c->tick({c->status() == GameClient::Status::Playing ? right : 0u, 0});
            c->updateView(now);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    for (auto& c : clients) {
        CHECK(c->status() == GameClient::Status::Playing);
        CHECK(c->game().players.size() == 2);
        CHECK(c->stats().snapshots > 20);
    }
    CHECK((*server)->playerCount() == 2);
    // Both moved right on the server, whichever way their input arrived.
    for (const auto& info : (*server)->clients()) {
        const Vec2 spawn = game->startScene.worldPosition(game->startScene.findByName("Player"));
        const Vec2 now = (*server)->session().scene().worldPosition((*server)->session().characterOf(info.player));
        CHECK(now.x > spawn.x + 20.0f);
    }
}
