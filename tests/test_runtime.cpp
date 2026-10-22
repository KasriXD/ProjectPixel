#include <doctest.h>

#include "px/core/FileSystem.hpp"
#include "px/project/Project.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/runtime/GameSession.hpp"
#include "px/runtime/Input.hpp"
#include "px/runtime/InputScript.hpp"

#include <algorithm>
#include <string>

using namespace px;
using namespace px::scene;
using namespace px::runtime;

namespace {

const std::string kExampleDir = std::string(PX_EXAMPLES_DIR) + "/platformer";

project::ProjectManifest testProject() {
    project::ProjectManifest m;
    m.name = "Test";
    m.settings.tickRate = 60;
    m.input.actions = {{"move_left", {Key::Left}}, {"move_right", {Key::Right}}, {"jump", {Key::Space}}};
    return m;
}

// A 40-tile-wide floor at row 10 (top y = 160), with a ledge ending at
// column 20 when `withGap` is set.
Scene floorScene(bool withGap = false) {
    Scene scene;
    const Entity level = scene.createEntity("Level");
    scene.set(level, Transform{});
    Tilemap map;
    map.width = 40;
    map.height = 12;
    map.tiles.assign(40 * 12, -1);
    for (int x = 0; x < 40; ++x) {
        if (withGap && x >= 20) continue;
        map.tiles[static_cast<std::size_t>(10 * 40 + x)] = 0;
        map.tiles[static_cast<std::size_t>(11 * 40 + x)] = 0;
    }
    scene.set(level, map);
    return scene;
}

Entity addPlayer(Scene& scene, Vec2 position) {
    const Entity p = scene.createEntity("Player");
    scene.set(p, Transform{position});
    scene.set(p, BoxCollider{{10, 14}, {0, 0}, false});
    scene.set(p, KinematicBody{});
    scene.set(p, PlatformerController{});
    scene.set(p, Player{});
    return p;
}

InputFrame hold(const GameSession& s, std::initializer_list<std::string_view> names, std::uint32_t previous = 0) {
    std::uint32_t held = 0;
    for (auto n : names) held |= 1u << s.actions().find(n);
    return InputFrame::fromHeld(held, previous);
}

void run(GameSession& s, int ticks, const InputFrame& frame = {}) {
    for (int i = 0; i < ticks; ++i) s.tick(frame);
}

const Transform& transformOf(GameSession& s, Entity e) { return *s.scene().tryGet<Transform>(e); }
const KinematicBody& bodyOf(GameSession& s, Entity e) { return *s.scene().tryGet<KinematicBody>(e); }

// Holds jump for `holdTicks` (from the ground) and returns the highest point
// reached, as the smallest Y of the player's position.
float jumpApex(int holdTicks) {
    Scene scene = floorScene();
    const Entity p = addPlayer(scene, {100, 153});
    GameSession s(testProject(), std::move(scene));
    run(s, 5);
    const std::uint32_t jumpBit = 1u << s.actions().find("jump");
    float apex = transformOf(s, p).position.y;
    std::uint32_t previous = 0;
    for (int i = 0; i < 90; ++i) {
        const std::uint32_t held = i < holdTicks ? jumpBit : 0u;
        s.tick(InputFrame::fromHeld(held, previous));
        previous = held;
        apex = std::min(apex, transformOf(s, p).position.y);
    }
    return apex;
}

} // namespace

TEST_CASE("Player falls, lands and stays grounded") {
    Scene scene = floorScene();
    const Entity p = addPlayer(scene, {100, 100});
    GameSession s(testProject(), std::move(scene));
    run(s, 60);
    CHECK(bodyOf(s, p).onGround);
    CHECK(transformOf(s, p).position.y == doctest::Approx(160.0f - 7.0f)); // collider bottom on the floor
    run(s, 120);
    CHECK(bodyOf(s, p).onGround);
    CHECK(transformOf(s, p).position.y == doctest::Approx(153.0f));
    CHECK(s.tickCount() == 180);
}

TEST_CASE("Horizontal movement accelerates to max speed and decelerates") {
    Scene scene = floorScene();
    const Entity p = addPlayer(scene, {100, 153});
    GameSession s(testProject(), std::move(scene));
    run(s, 2);
    run(s, 30, hold(s, {"move_right"}));
    CHECK(bodyOf(s, p).velocity.x == doctest::Approx(110.0f));
    CHECK(transformOf(s, p).position.x > 140.0f);
    CHECK_FALSE(s.scene().tryGet<Sprite>(p)); // no sprite: flipping is skipped safely

    run(s, 10); // release: deceleration brings the player to rest
    CHECK(bodyOf(s, p).velocity.x == doctest::Approx(0.0f));

    run(s, 30, hold(s, {"move_left"}));
    CHECK(bodyOf(s, p).velocity.x == doctest::Approx(-110.0f));
}

TEST_CASE("Jump height matches tuning and releasing early cuts the jump") {
    const float groundY = 153.0f;
    const float fullHeight = groundY - jumpApex(40);
    // v^2 / 2g = 300^2 / 1800 = 50 units, minus discretisation error.
    CHECK(fullHeight > 44.0f);
    CHECK(fullHeight <= 50.5f);

    const float shortHeight = groundY - jumpApex(4);
    CHECK(shortHeight < fullHeight * 0.6f);
    CHECK(shortHeight > 5.0f);
}

TEST_CASE("Coyote time allows a jump just after leaving a ledge") {
    // Ledge ends at x = 320. Walk off, then press jump a few ticks later.
    const auto jumpAfterLeaving = [](int ticksAfterLedge) {
        Scene scene = floorScene(true);
        const Entity p = addPlayer(scene, {300, 153});
        GameSession s(testProject(), std::move(scene));
        run(s, 2);
        const InputFrame right = hold(s, {"move_right"});
        int airborneTicks = 0;
        for (int i = 0; i < 200 && airborneTicks < ticksAfterLedge; ++i) {
            s.tick(right);
            if (!bodyOf(s, p).onGround) ++airborneTicks;
        }
        s.tick(hold(s, {"move_right", "jump"}, right.held));
        return bodyOf(s, p).velocity.y < -200.0f; // a jump started
    };
    CHECK(jumpAfterLeaving(1));
    CHECK(jumpAfterLeaving(3));
    CHECK_FALSE(jumpAfterLeaving(10)); // 0.08 s coyote window = ~5 ticks
}

TEST_CASE("A jump pressed just before landing is buffered") {
    Scene scene = floorScene();
    const Entity p = addPlayer(scene, {100, 100});
    GameSession s(testProject(), std::move(scene));
    const InputFrame jump = hold(s, {"jump"});
    // Fall until just above the floor (3 ticks of fall left).
    while (transformOf(s, p).position.y < 140.0f) s.tick({});
    CHECK_FALSE(bodyOf(s, p).onGround);
    s.tick(jump);                          // press while airborne
    for (int i = 0; i < 4; ++i) s.tick(InputFrame::fromHeld(jump.held, jump.held)); // keep holding
    CHECK(bodyOf(s, p).velocity.y < 0.0f);  // jumped right after landing
    CHECK(transformOf(s, p).position.y < 153.0f);
}

TEST_CASE("Walls stop the player") {
    Scene scene = floorScene();
    const Entity wall = scene.createEntity("Wall");
    scene.set(wall, Transform{{200, 120}});
    scene.set(wall, BoxCollider{{16, 80}, {}, false});
    const Entity p = addPlayer(scene, {150, 153});
    GameSession s(testProject(), std::move(scene));
    run(s, 120, hold(s, {"move_right"}));
    CHECK(transformOf(s, p).position.x == doctest::Approx(192.0f - 5.0f));
    CHECK(bodyOf(s, p).hitWall);
}

TEST_CASE("Collectibles are collected once, raise events and finish the level") {
    Scene scene = floorScene();
    const Entity p = addPlayer(scene, {100, 153});
    for (int i = 0; i < 2; ++i) {
        const Entity coin = scene.createEntity("Coin");
        scene.set(coin, Transform{{130.0f + 30.0f * static_cast<float>(i), 150.0f}});
        scene.set(coin, BoxCollider{{10, 12}, {}, true});
        scene.set(coin, Collectible{"coin", 5});
    }
    GameSession s(testProject(), std::move(scene));
    CHECK(s.state().collectiblesTotal == 2);
    (void)p;

    int collectedEvents = 0;
    int completeEvents = 0;
    const InputFrame right = hold(s, {"move_right"});
    for (int i = 0; i < 120; ++i) {
        s.tick(right);
        for (const auto& ev : s.events()) {
            if (ev.type == GameEvent::Type::Collected) {
                ++collectedEvents;
                CHECK(ev.kind == "coin");
                CHECK(ev.value == 5);
            }
            if (ev.type == GameEvent::Type::AllCollected) ++completeEvents;
        }
    }
    CHECK(collectedEvents == 2);
    CHECK(completeEvents == 1);
    CHECK(s.state().score == 10);
    CHECK(s.state().allCollected);
    CHECK(s.scene().registry().count<Collectible>() == 0);
    CHECK(s.scene().findByName("Coin").isNull());
}

TEST_CASE("Falling below the kill plane respawns the player at the spawn point") {
    Scene scene = floorScene(true);
    scene.settings().killPlaneY = 260.0f;
    const Entity p = addPlayer(scene, {300, 153});
    GameSession s(testProject(), std::move(scene));
    bool respawned = false;
    for (int i = 0; i < 240 && !respawned; ++i) {
        s.tick(hold(s, {"move_right"}));
        for (const auto& ev : s.events()) respawned |= ev.type == GameEvent::Type::PlayerRespawned;
    }
    CHECK(respawned);
    CHECK(s.state().respawns == 1);
    CHECK(transformOf(s, p).position == Vec2{300, 153});
    CHECK(bodyOf(s, p).velocity == Vec2{});
}

TEST_CASE("Camera follows its target and stays inside the level") {
    Scene scene = floorScene(); // 640 x 192 world
    const Entity p = addPlayer(scene, {20, 153});
    const Entity cam = scene.createEntity("Camera");
    Camera camera;
    camera.follow = scene.tryGet<EntityInfo>(p)->id;
    scene.set(cam, camera);
    GameSession s(testProject(), std::move(scene)); // view 320 x 180
    const auto& c = *s.scene().tryGet<Camera>(cam);
    CHECK(c.position.x == doctest::Approx(160.0f)); // clamped at the left edge
    CHECK(c.position.y == doctest::Approx(102.0f)); // 192 - 90
    run(s, 180, hold(s, {"move_right"}));
    CHECK(c.position.x == doctest::Approx(transformOf(s, p).position.x));
    run(s, 300, hold(s, {"move_right"}));
    CHECK(c.position.x == doctest::Approx(640.0f - 160.0f)); // clamped at the right edge
}

TEST_CASE("Sprite animation advances and loops") {
    Scene scene;
    const Entity e = scene.createEntity("Coin");
    scene.set(e, Transform{});
    scene.set(e, Sprite{});
    SpriteAnimation anim;
    anim.frames = {{0, 0, 16, 16}, {16, 0, 16, 16}, {32, 0, 16, 16}};
    anim.framesPerSecond = 10;
    scene.set(e, anim);
    GameSession s(testProject(), std::move(scene));
    run(s, 7); // 0.1167 s -> frame 1
    CHECK(s.scene().tryGet<Sprite>(e)->source.x == doctest::Approx(16.0f));
    run(s, 12); // 0.3167 s -> frame 3 -> wraps to 0
    CHECK(s.scene().tryGet<Sprite>(e)->source.x == doctest::Approx(0.0f));
}

TEST_CASE("Input actions, key bindings and frames") {
    const auto project = testProject();
    const ActionSet actions(project.input);
    CHECK(actions.size() == 3);
    CHECK(actions.find("jump") == 2);
    CHECK(actions.find("fly") == kInvalidAction);
    CHECK(actions.name(0) == "move_left");

    const KeyBindings bindings(project.input, actions);
    KeyStates keys{};
    keys[static_cast<std::size_t>(Key::Space)] = true;
    keys[static_cast<std::size_t>(Key::Left)] = true;
    CHECK(bindings.heldActions(keys) == ((1u << 0) | (1u << 2)));
    CHECK(bindings.actionsForKey(Key::Right) == (1u << 1));
    CHECK(bindings.actionsForKey(Key::Q) == 0u);

    const InputFrame f = InputFrame::fromHeld(0b101, 0b001);
    CHECK(f.isHeld(0));
    CHECK(f.isHeld(2));
    CHECK_FALSE(f.wasPressed(0));
    CHECK(f.wasPressed(2));
    CHECK_FALSE(f.isHeld(kInvalidAction));
}

TEST_CASE("Input scripts parse and play back with press edges") {
    const ActionSet actions(testProject().input);
    auto script = InputScript::parse("# comment\n2 move_right\n\n1 move_right+jump # trailing\n1 -\n", actions);
    REQUIRE(script.ok());
    CHECK(script->totalTicks() == 4);
    InputScriptPlayer player(std::move(script.value()));
    const InputFrame a = player.next();
    CHECK(a.isHeld(1));
    CHECK(a.wasPressed(1));
    const InputFrame b = player.next();
    CHECK(b.isHeld(1));
    CHECK_FALSE(b.wasPressed(1));
    const InputFrame c = player.next();
    CHECK(c.wasPressed(2));
    const InputFrame d = player.next();
    CHECK(d.held == 0u);
    CHECK(player.finished());

    CHECK_FALSE(InputScript::parse("x move_right", actions).ok());
    CHECK_FALSE(InputScript::parse("0 move_right", actions).ok());
    CHECK_FALSE(InputScript::parse("3 fly", actions).ok());
    CHECK_FALSE(InputScript::parse("3 move_right jump", actions).ok());
    CHECK_FALSE(InputScript::parse("3", actions).ok());
}

TEST_CASE("Example level: scripted playthrough collects every coin, deterministically") {
    DirectoryFileSystem fs(kExampleDir);
    auto manifest = project::loadProjectManifest(fs);
    REQUIRE(manifest.ok());
    auto scene = project::loadScene(fs, manifest.value(), manifest->startScene);
    REQUIRE(scene.ok());
    auto scriptText = fs.readText("tests/full_run.input");
    REQUIRE(scriptText.ok());

    const auto play = [&](Scene start) {
        GameSession session(manifest.value(), std::move(start));
        auto script = InputScript::parse(scriptText.value(), session.actions());
        REQUIRE(script.ok());
        InputScriptPlayer player(std::move(script.value()));
        while (!player.finished()) session.tick(player.next());
        const Entity p = session.player();
        return std::tuple{session.state().allCollected, session.state().score, session.state().respawns,
                          session.scene().tryGet<Transform>(p)->position, session.tickCount()};
    };
    const auto first = play(scene.value());
    const auto second = play(scene.value());
    CHECK(std::get<0>(first));
    CHECK(std::get<1>(first) == 12);
    CHECK(std::get<2>(first) == 0);
    // Same input, same result, bit for bit: required for replays and server reconciliation.
    CHECK(std::get<3>(first).x == std::get<3>(second).x);
    CHECK(std::get<3>(first).y == std::get<3>(second).y);
    CHECK(std::get<4>(first) == std::get<4>(second));
}
