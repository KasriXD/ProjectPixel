// Multiplayer in the game session: player templates, joining and leaving,
// per-player input, and the scripting API for players.

#include <doctest.h>

#include "px/runtime/GameSession.hpp"
#include "px/scripting/ScriptLibrary.hpp"

#include <memory>
#include <stdexcept>
#include <string>

using namespace px;
using namespace px::scene;
using namespace px::runtime;

namespace {

project::ProjectManifest mpProject() {
    project::ProjectManifest m;
    m.name = "MP";
    m.settings.tickRate = 60;
    m.input.actions = {{"move_left", {Key::Left}}, {"move_right", {Key::Right}}, {"jump", {Key::Space}},
                       {"use", {Key::E}}};
    return m;
}

// A floor at y = 160 and one Player template standing at x = 100.
Scene mpScene(int templates = 1) {
    Scene scene;
    const Entity level = scene.createEntity("Level");
    scene.set(level, Transform{});
    Tilemap map;
    map.width = 60;
    map.height = 12;
    map.tiles.assign(60 * 12, -1);
    for (int x = 0; x < 60; ++x) map.tiles[static_cast<std::size_t>(10 * 60 + x)] = 0;
    scene.set(level, map);
    for (int i = 0; i < templates; ++i) {
        const Entity p = scene.createEntity("Player");
        scene.set(p, Transform{{100.0f + 200.0f * static_cast<float>(i), 150.0f}});
        scene.set(p, Sprite{});
        scene.set(p, BoxCollider{{10, 14}, {0, 0}, false});
        scene.set(p, KinematicBody{});
        scene.set(p, PlatformerController{});
        scene.set(p, Player{});
        const Entity hat = scene.createEntity("Hat", p);
        scene.set(hat, Transform{{0, -10}});
    }
    const Entity cam = scene.createEntity("Camera");
    scene.set(cam, Camera{});
    return scene;
}

std::unique_ptr<GameSession> makeServerSession(Scene scene, std::shared_ptr<const scripting::ScriptLibrary> scripts = {}) {
    SessionOptions options;
    options.multiplayer = true;
    auto session = GameSession::create(mpProject(), std::move(scene), std::move(scripts), options);
    if (!session) throw std::runtime_error(session.error().message);
    return std::move(session.value());
}

InputFrame holding(const GameSession& s, std::string_view action, bool pressed = false) {
    const std::uint32_t bit = 1u << s.actions().find(action);
    return {bit, pressed ? bit : 0u};
}

float xOf(const GameSession& s, Entity e) { return s.scene().worldPosition(e).x; }

} // namespace

TEST_CASE("Multiplayer: Player entities become templates; each player gets a copy") {
    Scene scene = mpScene();
    PlayerTemplates templates = extractPlayerTemplates(scene);
    CHECK(templates.entries.size() == 1);
    CHECK(scene.registry().count<Player>() == 0);
    CHECK(scene.findByName("Hat").isNull()); // children go with the template
    CHECK(templates.entries[0].spawn == Vec2{100, 150});

    const Scene original = mpScene();
    auto session = makeServerSession(original);
    GameSession& s = *session;
    CHECK(s.multiplayer());
    CHECK(s.scene().registry().count<Player>() == 0);

    auto a = s.addPlayer(1, "Ada");
    auto b = s.addPlayer(2, "Bo");
    REQUIRE(a);
    REQUIRE(b);
    CHECK(s.players().size() == 2);
    CHECK(s.scene().registry().count<Player>() == 2);
    CHECK(s.scene().get<Player>(a.value()).owner == 1);
    CHECK(s.scene().get<Player>(b.value()).name == "Bo");
    CHECK(s.scene().worldPosition(a.value()) == Vec2{100, 150});
    CHECK(s.scene().children(a.value()).size() == 1); // its own hat
    CHECK(s.characterOf(2) == b.value());

    // Ids are derived, so they are the same in every session of the same scene.
    auto again = makeServerSession(original);
    auto a2 = again->addPlayer(1, "Ada");
    REQUIRE(a2);
    CHECK(again->scene().get<EntityInfo>(a2.value()).id == s.scene().get<EntityInfo>(a.value()).id);
    CHECK(s.scene().get<EntityInfo>(a.value()).id != s.scene().get<EntityInfo>(b.value()).id);

    CHECK_FALSE(s.addPlayer(1, "dup"));      // id in use
    CHECK_FALSE(s.addPlayer(kNoPlayer, "")); // not an id
}

TEST_CASE("Multiplayer: each character follows its own player's input") {
    auto session = makeServerSession(mpScene());
    GameSession& s = *session;
    const Entity a = s.addPlayer(1, "A").value();
    const Entity b = s.addPlayer(2, "B").value();
    for (int i = 0; i < 30; ++i) s.tick(); // land
    const float ax = xOf(s, a);
    const float bx = xOf(s, b);
    for (int i = 0; i < 30; ++i) {
        s.setPlayerInput(1, holding(s, "move_right"));
        s.setPlayerInput(2, holding(s, "move_left"));
        s.tick();
    }
    CHECK(xOf(s, a) > ax + 20.0f);
    CHECK(xOf(s, b) < bx - 20.0f);
    CHECK(s.scene().get<Sprite>(b).flipX);
    CHECK_FALSE(s.scene().get<Sprite>(a).flipX);

    // A player who leaves stops moving and their character is removed next tick.
    s.removePlayer(2);
    CHECK(s.players().size() == 1);
    s.tick();
    CHECK_FALSE(s.scene().isAlive(b));
    CHECK(s.scene().registry().count<Player>() == 1);
}

TEST_CASE("Multiplayer: several Player entities are several spawn points") {
    auto session = makeServerSession(mpScene(2));
    GameSession& s = *session;
    const Entity a = s.addPlayer(1, "A").value();
    const Entity b = s.addPlayer(2, "B").value();
    const Entity c = s.addPlayer(3, "C").value();
    CHECK(xOf(s, a) == doctest::Approx(100));
    CHECK(xOf(s, b) == doctest::Approx(300));
    // The third shares the first spawn point and stands next to the first player.
    CHECK(xOf(s, c) == doctest::Approx(120));
    CHECK(s.scene().get<Player>(c).spawnPoint.x == doctest::Approx(120));
}

TEST_CASE("Multiplayer: scripts see players join and leave, and per-player input") {
    auto library = std::make_shared<scripting::ScriptLibrary>();
    const assets::AssetId rulesId{Guid(7, 1)};
    REQUIRE(library->add(rulesId, "scripts/rules.luau", R"lua(
        local Rules = {}
        function Rules:Start()
            self.joined = 0
        end
        function Rules:PlayerJoined(player)
            self.joined += 1
            Game.setObjective("WELCOME " .. player.playerName .. " #" .. player.playerId)
            Game.addScore(1)
        end
        function Rules:PlayerLeft(player)
            Game.setObjective("BYE " .. player.playerName .. " LEFT " .. (Players.count() - 1))
        end
        function Rules:Tick()
            for _, p in Players.getAll() do
                if Input.wasPressed("use", p) then
                    Game.addScore(100 * p.playerId)
                end
            end
            if Input.isDown("jump") then
                Game.addScore(1000)
            end
        end
        return Rules
    )lua"));
    Scene scene = mpScene();
    const Entity rules = scene.createEntity("Rules");
    Scripts scripts;
    scripts.items.push_back({rulesId, {}});
    scene.set(rules, scripts);

    auto session = makeServerSession(std::move(scene), library);
    GameSession& s = *session;
    s.tick();
    CHECK(s.state().objective.empty());
    (void)s.addPlayer(1, "Ada");
    s.tick();
    CHECK(s.state().objective == "WELCOME Ada #1");
    CHECK(s.state().score == 1);
    (void)s.addPlayer(2, "Bo");
    s.tick();
    CHECK(s.state().objective == "WELCOME Bo #2");
    CHECK(s.state().score == 2);

    // Only player 2 presses "use": +200. Then player 1 holds jump: "any player" input.
    s.setPlayerInput(2, holding(s, "use", true));
    s.tick();
    CHECK(s.state().score == 202);
    s.setPlayerInput(2, {});
    s.setPlayerInput(1, holding(s, "jump"));
    s.tick();
    CHECK(s.state().score == 1202);
    s.setPlayerInput(1, {});

    s.removePlayer(1);
    s.tick();
    CHECK(s.state().objective == "BYE Ada LEFT 1");
    CHECK(s.state().scriptErrors == 0);
}

TEST_CASE("Playing alone: PlayerJoined runs once at the start, input needs no player") {
    auto library = std::make_shared<scripting::ScriptLibrary>();
    const assets::AssetId id{Guid(7, 2)};
    REQUIRE(library->add(id, "scripts/hello.luau", R"lua(
        local Hello = {}
        function Hello:PlayerJoined(player)
            Game.setObjective("HELLO " .. player.playerName .. " " .. player.playerId .. " " .. Players.count())
        end
        function Hello:Tick()
            local p = Players.getAll()[1]
            if Input.isDown("move_right", p) and Input.isDown("move_right") then
                Game.addScore(1)
            end
        end
        return Hello
    )lua"));
    Scene scene = mpScene();
    const Entity e = scene.createEntity("Hello");
    Scripts scripts;
    scripts.items.push_back({id, {}});
    scene.set(e, scripts);
    auto created = GameSession::create(mpProject(), std::move(scene), library);
    REQUIRE(created);
    GameSession& s = *created.value();
    CHECK_FALSE(s.multiplayer());
    CHECK_FALSE(s.addPlayer(1, "x"));
    s.tick(holding(s, "move_right"));
    CHECK(s.state().objective == "HELLO Player 0 1");
    CHECK(s.state().score == 1);
    s.tick(holding(s, "move_right"));
    CHECK(s.state().score == 2);
    CHECK(s.state().scriptErrors == 0);
}
