// A dedicated server and clients over the loopback network: joining,
// movement, prediction, scripted gameplay through the network, leaving,
// refusal and level restarts.

#include <doctest.h>

#include "px/core/FileSystem.hpp"
#include "px/net/GameClient.hpp"
#include "px/net/ConnectToken.hpp"
#include "px/net/GameServer.hpp"
#include "px/net/Loopback.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/runtime/InputScript.hpp"

#include <cmath>
#include <fstream>
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
                          const runtime::LoadedGame* clientGame = nullptr, const std::string& token = {}) {
        ClientOptions options;
        options.playerName = name;
        options.token = token;
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
    // Idle snapshots are small: only the animated coins and the 8 drifting clouds change.
    CHECK(h.server->lastSnapshotBytes() < 520);
}

TEST_CASE("Netgame: a networked player completes the top-down level over a lossy link") {
    Harness h(loadExample("topdown"));
    LinkConditions link;
    link.latencyMs = 50;
    link.jitterMs = 10;
    link.lossPercent = 3;
    link.duplicatePercent = 1;
    GameClient& hero = h.addClient("Hero", link);
    h.runUntilPlaying(); // the hero takes the spawn point the route starts from
    GameClient& watcher = h.addClient("Watcher");
    h.runUntilPlaying();
    DirectoryFileSystem files(std::string(PX_EXAMPLES_DIR) + "/topdown/tests");
    auto route = files.readText("full_run.input");
    REQUIRE(route);
    h.play(0, route.value());
    bool complete = false;
    int sparklesSeen = 0; // collected gems spawn a Sparkle prefab on the server (World.spawn)
    bool sparkleVisible = false;
    for (int i = 0; i < 1200 && !complete; ++i) {
        h.step();
        complete = h.server->session().state().levelComplete;
        const bool visible = !watcher.view().findByName("Sparkle").isNull();
        if (visible && !sparkleVisible) ++sparklesSeen;
        sparkleVisible = visible;
    }
    CHECK(complete);
    CHECK(sparklesSeen >= 3); // spawned entities reach the other client, and go away again
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

TEST_CASE("Netgame: platform servers take only valid connect tokens and keep saved data") {
    auto key = crypto::SigningKey::generate();
    auto otherKey = crypto::SigningKey::generate();
    REQUIRE(key.ok());
    REQUIRE(otherKey.ok());
    auto store = std::make_shared<MemoryPlayerStore>();
    runtime::PlayerData saved;
    REQUIRE(saved.set("coins", 5.0).ok());
    store->m_saved["42"] = saved;
    ServerOptions options;
    options.platformKey = key->publicKey;
    options.sessionId = "session1";
    options.playerStore = store;
    options.saveIntervalSeconds = 1.0;
    Harness h(loadExample("platformer"), options);
    int nonce = 0;
    const auto token = [&](const std::string& user, const std::string& name, const std::string& session = "session1",
                           std::int64_t expiresIn = 60, const crypto::SigningKey* signer = nullptr) {
        ConnectToken t{session, user, name, unixTimeNow() + expiresIn, "nonce-" + std::to_string(++nonce) + "-abcdefgh"};
        return signConnectToken(t, signer != nullptr ? *signer : key.value());
    };
    const std::string adaToken = token("42", "Ada");
    GameClient& ada = h.addClient("Somebody Else", {}, nullptr, adaToken);
    GameClient& none = h.addClient("Mallory");
    GameClient& elsewhere = h.addClient("x", {}, nullptr, token("43", "Bob", "session2"));
    GameClient& expired = h.addClient("x", {}, nullptr, token("44", "Cy", "session1", -5));
    GameClient& forged = h.addClient("x", {}, nullptr, token("45", "Dee", "session1", 60, &otherKey.value()));
    GameClient& replay = h.addClient("x", {}, nullptr, adaToken);
    h.run(90);
    REQUIRE(ada.status() == GameClient::Status::Playing);
    CHECK(h.server->clients().size() == 1);
    CHECK(h.server->clients().front().userId == "42");
    CHECK(ada.game().players.front().name == "Ada"); // the token's name, not the one sent
    for (GameClient* refused : {&none, &elsewhere, &expired, &forged, &replay}) {
        CHECK(refused->status() == GameClient::Status::Disconnected);
    }
    CHECK(none.statusText().find("join ticket") != std::string::npos);
    CHECK(elsewhere.statusText().find("another game server") != std::string::npos);
    CHECK(expired.statusText().find("expired") != std::string::npos);
    CHECK(forged.statusText().find("not signed") != std::string::npos);
    const bool replayRefused = replay.statusText().find("already") != std::string::npos;
    CHECK(replayRefused);

    // Ada's saved data was loaded before she joined; collecting coins adds to it.
    const auto* slot = h.server->session().findPlayer(ada.player());
    REQUIRE(slot != nullptr);
    REQUIRE(slot->data.get("coins") != nullptr);
    CHECK(std::get<double>(*slot->data.get("coins")) == 5.0);
    const auto route = readLocalFile(std::string(PX_EXAMPLES_DIR) + "/platformer/tests/full_run.input");
    REQUIRE(route.ok());
    const std::string text(route->begin(), route->end());
    h.play(0, text);
    h.run(1100);
    const int collected = h.server->session().state().score; // each coin is worth 1
    CHECK(collected >= 5);
    CHECK(store->saves >= 1); // saved while playing
    CHECK(std::get<double>(*store->m_saved["42"].get("coins")) == 5.0 + collected);

    // Leaving saves one last time.
    ada.disconnect();
    h.run(20);
    CHECK(store->finalSaves == 1);
    CHECK(h.server->playerCount() == 0);
}

TEST_CASE("Netgame: saved data that cannot be loaded keeps the player out") {
    struct FailingStore final : IPlayerStore {
        void beginLoad(const std::string&) override {}
        std::optional<Result<runtime::PlayerData>> takeLoaded(const std::string&) override {
            return Result<runtime::PlayerData>(makeError(ErrorCode::IoError, "platform down"));
        }
        void save(const std::string&, const runtime::PlayerData&, bool) override { ++saves; }
        void release(const std::string&) override { ++releases; }
        void flush(double) override {}
        int saves = 0;
        int releases = 0;
    };
    auto key = crypto::SigningKey::generate();
    REQUIRE(key.ok());
    auto store = std::make_shared<FailingStore>();
    ServerOptions options;
    options.platformKey = key->publicKey;
    options.sessionId = "s";
    options.playerStore = store;
    Harness h(loadExample("topdown"), options);
    ConnectToken t{"s", "7", "Eve", unixTimeNow() + 60, "abcdefghijk"};
    GameClient& eve = h.addClient("Eve", {}, nullptr, signConnectToken(t, key.value()));
    h.run(60);
    CHECK(eve.status() == GameClient::Status::Disconnected);
    CHECK(eve.statusText().find("nothing was lost") != std::string::npos);
    CHECK(store->saves == 0); // never overwrite data that was not loaded
    CHECK(store->releases == 1);
}

TEST_CASE("Netgame: sounds and music reach the players they are for, in step with the view") {
    runtime::LoadedGame game = loadExample("platformer");
    const assets::AssetId beep{Guid(0x50000000u, 1)};
    const assets::AssetId song{Guid(0x50000000u, 2)};
    REQUIRE(game.manifest.assets.add({beep, assets::AssetType::Sound, "Beep", "sounds/beep.wav"}));
    REQUIRE(game.manifest.assets.add({song, assets::AssetType::Sound, "Song", "sounds/song.ogg"}));
    auto scripts = std::make_shared<scripting::ScriptLibrary>(*game.scripts);
    const assets::AssetId jukebox{Guid(0x5c000000u, 99)};
    REQUIRE(scripts->add(jukebox, "scripts/jukebox.luau", R"lua(
        local S = {}
        local done = false
        function S:Tick()
            if done or Players.count() < 2 then return end
            done = true
            local players = Players.getAll()
            Sound.play("Beep")                           -- everyone
            Sound.playFor(players[1], "Beep", 0.5)       -- only the first player
            players[1]:playSound("Beep", 1, 2)           -- from the first player's character
            Sound.playAt("Beep", Vec2(100, 50), 0.25)    -- at a place in the world
            Sound.playMusic("Song", 0.6, 1)
            Sound.playMusicFor(players[2], "Beep", 1, 0) -- the second player hears their own
        end
        return S
    )lua"));
    game.scripts = scripts;
    const Entity box = game.startScene.createEntity("Jukebox");
    game.startScene.set(box, Transform{});
    game.startScene.set(box, Scripts{{AttachedScript{jukebox, {}}}});

    Harness h(std::move(game));
    LinkConditions link;
    link.latencyMs = 30;
    GameClient& ada = h.addClient("Ada", link);
    GameClient& bo = h.addClient("Bo", link);
    h.runUntilPlaying();
    std::vector<runtime::SoundEvent> heardByAda;
    std::vector<runtime::SoundEvent> heardByBo;
    for (int i = 0; i < 120; ++i) {
        h.step();
        for (auto& s : ada.takeSounds()) heardByAda.push_back(s);
        for (auto& s : bo.takeSounds()) heardByBo.push_back(s);
    }
    CHECK(h.server->session().state().scriptErrors == 0);

    REQUIRE(heardByAda.size() == 4);
    REQUIRE(heardByBo.size() == 3);
    const Guid adaCharacter = ada.view().get<EntityInfo>(ada.character()).id;
    // Ada hears her own character's sound first: it is not held back for the view.
    CHECK(heardByAda[0].entity == adaCharacter);
    CHECK(heardByAda[0].pitch == 2.0f);
    REQUIRE(heardByAda[0].position);
    int forAdaOnly = 0;
    for (const auto& s : heardByAda) forAdaOnly += s.volume == 0.5f ? 1 : 0;
    CHECK(forAdaOnly == 1);
    for (const auto& s : heardByBo) CHECK(s.volume != 0.5f);
    // Bo hears Ada's character's sound from where Ada's character is in his view.
    bool fromAda = false;
    for (const auto& s : heardByBo) {
        if (s.entity.isNil()) continue;
        fromAda = true;
        bool known = false;
        for (const Entity e : bo.view().entities()) known = known || bo.view().get<EntityInfo>(e).id == s.entity;
        CHECK(known);
    }
    CHECK(fromAda);
    bool placed = false;
    for (const auto& s : heardByBo) placed = placed || (s.position && *s.position == Vec2{100, 50} && s.volume == 0.25f);
    CHECK(placed);

    // Music: everyone's for Ada, Bo's own for Bo.
    CHECK(ada.music().sound == song);
    CHECK(ada.music().volume == doctest::Approx(0.6f));
    CHECK(bo.music().sound == beep);
    CHECK(bo.music().fadeSeconds == 0.0f);
}
