// A dedicated server and clients over the loopback network: joining,
// movement, prediction, scripted gameplay through the network, leaving,
// refusal and level restarts.

#include <doctest.h>

#include "px/core/FileSystem.hpp"
#include "px/net/GameClient.hpp"
#include "px/net/GameServer.hpp"
#include "px/net/Loopback.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/runtime/InputScript.hpp"

#include <cmath>
#include <optional>
#include <stdexcept>

using namespace px;
using namespace px::net;
using namespace px::scene;

namespace {

runtime::LoadedGame loadExample(const std::string& name) {
    DirectoryFileSystem files(std::string(PX_EXAMPLES_DIR) + "/" + name);
    auto game = runtime::loadGame(files);
    if (!game) throw std::runtime_error(game.error().message);
    return std::move(game.value());
}

constexpr std::uint16_t kPort = 7777;

// Runs a server and clients in lockstep over a loopback network.
struct Harness {
    LoopbackNetwork net{7};
    runtime::LoadedGame game;
    std::unique_ptr<GameServer> server;
    std::vector<std::unique_ptr<GameClient>> clients;
    std::vector<std::optional<runtime::InputScriptPlayer>> scripts;
    std::vector<runtime::InputFrame> held; // per client, when no script
    double time = 0.0;

    explicit Harness(runtime::LoadedGame g, ServerOptions options = {}) : game(std::move(g)) {
        auto endpoint = net.listen(kPort);
        if (!endpoint) throw std::runtime_error(endpoint.error().message);
        auto s = GameServer::create(game, std::move(endpoint.value()), options);
        if (!s) throw std::runtime_error(s.error().message);
        server = std::move(s.value());
    }

    GameClient& addClient(const std::string& name, const LinkConditions& link = {},
                          const runtime::LoadedGame* clientGame = nullptr) {
        ClientOptions options;
        options.playerName = name;
        auto c = GameClient::create(clientGame != nullptr ? *clientGame : game, net.connect(kPort, link), options);
        if (!c) throw std::runtime_error(c.error().message);
        clients.push_back(std::move(c.value()));
        scripts.emplace_back();
        held.emplace_back();
        return *clients.back();
    }

    void play(std::size_t client, const std::string& scriptText) {
        auto parsed = runtime::InputScript::parse(scriptText, server->session().actions());
        if (!parsed) throw std::runtime_error(parsed.error().message);
        scripts[client].emplace(std::move(parsed.value()));
    }

    void hold(std::size_t client, std::initializer_list<std::string_view> actions) {
        std::uint32_t bits = 0;
        for (auto a : actions) bits |= 1u << server->session().actions().find(a);
        held[client] = runtime::InputFrame{bits, bits & ~held[client].held};
    }

    // One tick of everything (1 / tickRate seconds).
    void step() {
        time += 1.0 / server->tickRate();
        net.setTime(time);
        server->tick();
        for (std::size_t i = 0; i < clients.size(); ++i) {
            GameClient& c = *clients[i];
            c.receive(time);
            runtime::InputFrame frame = held[i];
            if (scripts[i] && c.status() == GameClient::Status::Playing) frame = scripts[i]->next();
            c.tick(frame);
            held[i].pressed = 0;
            c.updateView(time);
        }
    }
    void run(int ticks) {
        for (int i = 0; i < ticks; ++i) step();
    }
    void runUntilPlaying() {
        for (int i = 0; i < 600; ++i) {
            bool all = true;
            for (const auto& c : clients) all = all && c->status() == GameClient::Status::Playing;
            if (all) return;
            step();
        }
        throw std::runtime_error("clients did not join");
    }

    Vec2 serverPosition(runtime::PlayerId id) const {
        const Entity e = server->session().characterOf(id);
        return server->session().scene().worldPosition(e);
    }
    // Where client `viewer` shows the character of player `id`.
    std::optional<Vec2> viewPosition(std::size_t viewer, runtime::PlayerId id) {
        Scene& view = clients[viewer]->view();
        for (const Entity e : view.entities()) {
            const auto* p = view.tryGet<Player>(e);
            if (p != nullptr && p->owner == id) return view.worldPosition(e);
        }
        return std::nullopt;
    }
};

bool near(Vec2 a, Vec2 b, float tolerance) { return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance; }

} // namespace

TEST_CASE("Netgame: two players join, move independently and see each other") {
    Harness h(loadExample("platformer"));
    LinkConditions link;
    link.latencyMs = 40;
    GameClient& ada = h.addClient("Ada", link);
    GameClient& bo = h.addClient("Bo", link);
    CHECK(ada.status() == GameClient::Status::Connecting);
    h.runUntilPlaying();
    CHECK(ada.player() != bo.player());
    CHECK(h.server->playerCount() == 2);
    h.run(60); // land and settle
    CHECK(ada.game().players.size() == 2);
    CHECK(ada.game().players[1].name == "Bo");

    const Vec2 adaStart = h.serverPosition(ada.player());
    h.hold(0, {"move_right"});
    h.hold(1, {"jump"});
    h.run(45);
    h.hold(0, {});
    h.hold(1, {});
    h.run(90); // everything arrives and settles

    const Vec2 adaNow = h.serverPosition(ada.player());
    CHECK(adaNow.x > adaStart.x + 40.0f);
    // Ada's own (predicted) view and Bo's (interpolated) view agree with the server.
    const auto adaSeenByAda = h.viewPosition(0, ada.player());
    const auto adaSeenByBo = h.viewPosition(1, ada.player());
    const auto boSeenByAda = h.viewPosition(0, bo.player());
    REQUIRE(adaSeenByAda);
    REQUIRE(adaSeenByBo);
    REQUIRE(boSeenByAda);
    CHECK(near(*adaSeenByAda, adaNow, 0.05f));
    CHECK(near(*adaSeenByBo, adaNow, 0.05f));
    CHECK(near(*boSeenByAda, h.serverPosition(bo.player()), 0.05f));
    // Prediction runs the server's own code: on a steady link it never needs correcting.
    CHECK(ada.stats().corrections == 0);
    CHECK(ada.stats().snapshots > 50);
    CHECK(ada.character() != kNullEntity);
    // The camera follows the own character.
    const Entity cam = ada.view().findByName("Main Camera");
    REQUIRE_FALSE(cam.isNull());
    CHECK(ada.view().get<Camera>(cam).follow == ada.view().get<EntityInfo>(ada.character()).id);
    // Idle snapshots are small (animated coins and nothing else).
    CHECK(h.server->lastSnapshotBytes() < 400);
}

TEST_CASE("Netgame: a networked player completes the top-down level over a lossy link") {
    Harness h(loadExample("topdown"));
    LinkConditions link;
    link.latencyMs = 50;
    link.jitterMs = 10;
    link.lossPercent = 3;
    link.duplicatePercent = 1;
    GameClient& hero = h.addClient("Hero", link);
    GameClient& watcher = h.addClient("Watcher");
    h.runUntilPlaying();
    DirectoryFileSystem files(std::string(PX_EXAMPLES_DIR) + "/topdown/tests");
    auto route = files.readText("full_run.input");
    REQUIRE(route);
    h.play(0, route.value());
    bool complete = false;
    for (int i = 0; i < 1200 && !complete; ++i) {
        h.step();
        complete = h.server->session().state().levelComplete;
    }
    CHECK(complete);
    CHECK(h.server->session().state().scriptErrors == 0);
    h.run(30);
    // Both clients were told; gems the hero took are gone from the watcher's view too.
    CHECK(watcher.game().levelComplete);
    CHECK(hero.game().score == h.server->session().state().score);
    CHECK(watcher.view().findByName("Gold Key").isNull());
    CHECK(watcher.view().findByName("Gate").isNull());
    MESSAGE("hero corrections: " << hero.stats().corrections << ", snapshots dropped: "
                                 << hero.stats().snapshotsDropped);
}

TEST_CASE("Netgame: leaving, refusing and restarting") {
    ServerOptions options;
    options.maxPlayers = 2;
    options.restartDelaySeconds = 0.5;
    Harness h(loadExample("topdown"), options);
    GameClient& a = h.addClient("A");
    GameClient& b = h.addClient("B");
    h.runUntilPlaying();
    h.run(10);

    SUBCASE("the server is full") {
        GameClient& c = h.addClient("C");
        h.run(10);
        CHECK(c.status() == GameClient::Status::Disconnected);
        CHECK(c.statusText().find("full") != std::string::npos);
        CHECK(h.server->playerCount() == 2);
    }

    SUBCASE("a different copy of the game is refused") {
        runtime::LoadedGame modified = h.game;
        modified.startScene.get<Transform>(modified.startScene.findByName("Gold Key")).position.x += 1.0f;
        GameClient& c = h.addClient("C", {}, &modified);
        h.run(10);
        CHECK(c.status() == GameClient::Status::Disconnected);
        CHECK(c.statusText().find("different version") != std::string::npos);
    }

    SUBCASE("a player who leaves disappears for the others") {
        CHECK(h.viewPosition(0, b.player()).has_value());
        b.disconnect();
        h.run(20);
        CHECK(h.server->playerCount() == 1);
        CHECK_FALSE(h.viewPosition(0, b.player()).has_value());
        CHECK(a.game().players.size() == 1);
    }

    SUBCASE("the level restarts after it is complete") {
        const std::uint32_t epoch = h.server->epoch();
        h.server->session().scene().get<Transform>(h.server->session().characterOf(a.player())).position =
            h.server->session().scene().worldPosition(h.server->session().scene().findByName("Exit Stairs"));
        h.run(5);
        CHECK(h.server->session().state().levelComplete);
        h.run(20);
        CHECK(a.game().restartTicks >= 0);
        h.run(40);
        CHECK(h.server->epoch() == epoch + 1);
        CHECK_FALSE(h.server->session().state().levelComplete);
        CHECK(h.server->playerCount() == 2);
        h.run(20);
        CHECK_FALSE(a.game().levelComplete);
        CHECK(a.character() != kNullEntity);
        CHECK(b.character() != kNullEntity);
    }

    SUBCASE("garbage from a client gets it disconnected") {
        // A raw endpoint that speaks nonsense after connecting.
        auto rogue = h.net.connect(kPort);
        h.run(2);
        std::vector<NetEvent> events;
        rogue->poll(events);
        const std::vector<std::uint8_t> junk{99, 1, 2, 3};
        CHECK(rogue->send(1, Delivery::Reliable, junk));
        h.run(2);
        events.clear();
        rogue->poll(events);
        bool refused = false;
        for (const auto& e : events) refused = refused || e.type == NetEvent::Type::Disconnected;
        CHECK(refused);
        CHECK(h.server->playerCount() == 2);
    }

    SUBCASE("a silent connection times out") {
        auto silent = h.net.connect(kPort);
        h.run(static_cast<int>(6 * h.server->tickRate()));
        std::vector<NetEvent> events;
        silent->poll(events);
        bool closed = false;
        for (const auto& e : events) closed = closed || (e.type == NetEvent::Type::Disconnected &&
                                                         e.reason == static_cast<std::uint32_t>(DisconnectReason::Timeout));
        CHECK(closed);
    }
}
