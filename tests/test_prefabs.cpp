// Prefabs: the file format, instances and their ids, updating instances
// while keeping their own changes, spawning from scripts, and Studio's
// workflow (save as prefab, place, apply, revert).

#include <doctest.h>

#include "px/core/FileSystem.hpp"
#include "px/core/Log.hpp"
#include "px/editor/EditorProject.hpp"
#include "px/editor/Prefabs.hpp"
#include "px/editor/SceneDocument.hpp"
#include "px/project/Prefab.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/runtime/GameSession.hpp"
#include "px/scene/ComponentInfo.hpp"
#include "px/scripting/ScriptLibrary.hpp"

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace px;
using namespace px::scene;
using namespace px::project;

namespace {

template <class T>
T must(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result.value());
}
void must(const Status& status) {
    if (!status) throw std::runtime_error(status.error().message);
}
template <class T>
T& need(T* component) {
    if (component == nullptr) throw std::runtime_error("expected component is missing");
    return *component;
}
bool contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

const assets::AssetId kPrefabId{Guid(0x9f000000u, 1)};
const assets::AssetId kTexture{Guid(0x7e000000u, 1)};

const Guid& idOf(const Scene& scene, Entity e) { return scene.get<EntityInfo>(e).id; }

// A small prefab: "Coin" (sprite, trigger, tags) with a child "Glow" whose
// Camera-free reference points back at the coin.
Scene coinPrefab() {
    Scene p;
    const Entity coin = p.createEntity("Coin");
    p.set(coin, Transform{});
    Sprite sprite;
    sprite.texture = kTexture;
    sprite.size = {10, 10};
    p.set(coin, sprite);
    p.set(coin, BoxCollider{{10, 10}, {}, true});
    p.set(coin, Tags{{"coin"}});
    const Entity glow = p.createEntity("Glow", coin);
    p.set(glow, Transform{{0, -4}});
    p.set(glow, Camera{idOf(p, coin), 1.0f, true, {}}); // an internal reference
    return p;
}

Entity child(const Scene& scene, Entity parent, std::string_view name) {
    for (const Entity c : scene.children(parent)) {
        if (scene.get<EntityInfo>(c).name == name) return c;
    }
    return kNullEntity;
}

// A fresh folder for one test, removed afterwards.
class TempFolder {
public:
    TempFolder() : m_path(std::filesystem::temp_directory_path() / ("px-prefab-test-" + Guid::generate().toString())) {
        std::filesystem::create_directories(m_path);
    }
    ~TempFolder() {
        std::error_code ec;
        std::filesystem::remove_all(m_path, ec);
    }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return m_path; }

private:
    std::filesystem::path m_path;
};

} // namespace

TEST_CASE("Prefabs: the file format round-trips and refuses broken files") {
    const Scene content = coinPrefab();
    const std::string text = serializePrefab(content);
    CHECK(contains(text, "\"format\": \"px.prefab\""));
    const Scene loaded = must(parsePrefab(text, "coin.prefab.json"));
    CHECK(serializePrefab(loaded) == text);
    CHECK(loaded.entityCount() == 2);

    const auto error = [](std::string_view json) {
        auto r = parsePrefab(json, "bad.prefab.json");
        return r ? std::string() : r.error().message;
    };
    CHECK(contains(error(R"({"format": "px.scene", "formatVersion": 1, "sceneFormatVersion": 3, "entities": []})"),
                   "is this a prefab file?"));
    CHECK(contains(error(R"({"format": "px.prefab", "formatVersion": 9, "sceneFormatVersion": 3, "entities": []})"),
                   "update the engine"));
    CHECK(contains(error(R"({"format": "px.prefab", "formatVersion": 1, "sceneFormatVersion": 3, "entities": []})"),
                   "at least one entity"));
    CHECK(contains(error(R"({"format": "px.prefab", "formatVersion": 1, "sceneFormatVersion": 3, "entities": [
        {"id": "00000000-0000-4000-8000-000000000001", "name": "A", "components": {}},
        {"id": "00000000-0000-4000-8000-000000000002", "name": "B", "components": {}}]})"),
                   "exactly one root"));
    // Entities written by an older scene format are migrated (version 2: one "Script").
    const Scene migrated = must(parsePrefab(R"({"format": "px.prefab", "formatVersion": 1, "sceneFormatVersion": 2,
        "entities": [{"id": "00000000-0000-4000-8000-000000000001", "name": "A", "components": {
            "Script": {"script": "00000000-0000-4000-8000-0000000000aa"}}}]})",
                                            "old.prefab.json"));
    CHECK(need(migrated.tryGet<Scripts>(migrated.entities().front())).items.size() == 1);
}

TEST_CASE("Prefabs: instances derive their ids from the root and point at their own entities") {
    const Scene prefab = coinPrefab();
    Scene scene;
    const Guid rootId = Guid::generate();
    const Entity coin = must(instantiatePrefab(scene, prefab, kPrefabId, rootId));
    CHECK(idOf(scene, coin) == rootId);
    CHECK(need(scene.tryGet<PrefabInstance>(coin)).prefab == kPrefabId);
    const Entity glow = child(scene, coin, "Glow");
    REQUIRE_FALSE(glow.isNull());
    const Guid key = instanceKey(rootId, idOf(prefab, prefab.entities().front()));
    CHECK(idOf(scene, glow) == instanceEntityId(key, idOf(prefab, prefab.entities()[1])));
    CHECK(need(scene.tryGet<Camera>(glow)).follow == rootId); // the reference points at the copy

    // A second instance does not collide; the same root id twice does.
    CHECK(instantiatePrefab(scene, prefab, kPrefabId, Guid::generate()).ok());
    CHECK_FALSE(instantiatePrefab(scene, prefab, kPrefabId, rootId).ok());
    CHECK(findInstances(scene, kPrefabId).size() == 2);

    // Copying an instance (paste, duplicate) keeps it an instance: re-keyed, not random.
    const Entity copy = copyEntities(scene, scene, coin);
    REQUIRE_FALSE(copy.isNull());
    const Guid copyKey = instanceKey(idOf(scene, copy), idOf(prefab, prefab.entities().front()));
    const Entity copyGlow = child(scene, copy, "Glow");
    CHECK(idOf(scene, copyGlow) == instanceEntityId(copyKey, idOf(prefab, prefab.entities()[1])));
    CHECK(need(scene.tryGet<Camera>(copyGlow)).follow == idOf(scene, copy));
}

TEST_CASE("Prefabs: a prefab made from entities can be instantiated back exactly") {
    Scene scene;
    const Entity outside = scene.createEntity("Outside");
    const Entity box = scene.createEntity("Box");
    scene.set(box, Transform{{50, 60}, 0.0f, {1, 1}});
    scene.set(box, Tags{{"crate"}});
    const Entity lid = scene.createEntity("Lid", box);
    scene.set(lid, Transform{{0, -8}});
    scene.set(lid, Camera{idOf(scene, outside), 1.0f, true, {}}); // points outside the tree

    const Guid prefabRoot = Guid::generate();
    std::size_t cleared = 0;
    const Scene content = prefabFromEntities(scene, box, prefabRoot, &cleared);
    CHECK(cleared == 1);
    REQUIRE(content.entityCount() == 2);
    const Entity root = content.entities().front();
    CHECK(idOf(content, root) == prefabRoot);
    CHECK(need(content.tryGet<Transform>(root)).position == Vec2{0, 0});
    CHECK(need(content.tryGet<Camera>(content.entities()[1])).follow.isNil());
    CHECK(parsePrefab(serializePrefab(content), "box.prefab.json").ok());

    // The original tree is already an instance of it: the ids line up.
    const Guid key = instanceKey(idOf(scene, box), prefabRoot);
    CHECK(instanceEntityId(key, idOf(content, content.entities()[1])) == idOf(scene, lid));
}

TEST_CASE("Prefabs: updating an instance keeps its own changes and takes the prefab's") {
    const Scene before = coinPrefab();
    Scene scene;
    const Entity coin = must(instantiatePrefab(scene, before, kPrefabId, Guid::generate()));
    const Entity other = scene.createEntity("Watcher");
    // This instance's own changes:
    need(scene.tryGet<Transform>(coin)).position = {100, 40}; // placed somewhere
    scene.get<EntityInfo>(coin).name = "Big Coin";             // renamed
    need(scene.tryGet<Sprite>(coin)).size = {20, 20};           // bigger
    scene.set(coin, KinematicBody{});                            // a component added
    scene.registry().remove<Tags>(coin);                         // a component removed
    const Entity extra = scene.createEntity("Sparkle", coin);    // an entity added
    scene.set(extra, Transform{{3, 3}});
    const Entity glow = child(scene, coin, "Glow");
    scene.set(other, Camera{idOf(scene, glow), 1.0f, true, {}}); // a reference into the instance

    // The prefab changes: new tint, collider size, a new child, and Glow renamed.
    Scene after = before;
    const Entity pRoot = after.entities().front();
    need(after.tryGet<Sprite>(pRoot)).tint = Color{255, 0, 0, 255};
    need(after.tryGet<Sprite>(pRoot)).size = {12, 12};
    need(after.tryGet<BoxCollider>(pRoot)).size = {12, 12};
    const Entity shadow = after.createEntity("Shadow", pRoot);
    after.set(shadow, Transform{{0, 6}});
    after.get<EntityInfo>(after.entities()[1]).name = "Halo";

    const Guid coinId = idOf(scene, coin);
    const auto update = must(updateInstance(scene, coin, before, after));
    const Entity root = update.root;
    CHECK(idOf(scene, root) == coinId);
    CHECK(scene.get<EntityInfo>(root).name == "Big Coin");
    CHECK(need(scene.tryGet<Transform>(root)).position == Vec2{100, 40});
    CHECK(need(scene.tryGet<Sprite>(root)).size == Vec2{20, 20});           // kept: changed here
    CHECK(need(scene.tryGet<Sprite>(root)).tint == Color{255, 0, 0, 255}); // taken: unchanged here
    CHECK(need(scene.tryGet<BoxCollider>(root)).size == Vec2{12, 12});
    CHECK(scene.tryGet<KinematicBody>(root) != nullptr);
    CHECK(scene.tryGet<Tags>(root) == nullptr);
    CHECK(need(scene.tryGet<PrefabInstance>(root)).prefab == kPrefabId);
    CHECK_FALSE(child(scene, root, "Halo").isNull());
    CHECK_FALSE(child(scene, root, "Shadow").isNull());
    CHECK_FALSE(child(scene, root, "Sparkle").isNull());
    CHECK(update.keptEntities == 1);
    // The reference from outside still finds the (renamed) child.
    CHECK(need(scene.tryGet<Camera>(other)).follow == idOf(scene, child(scene, root, "Halo")));
    CHECK(update.warnings.empty());

    // Reverting throws the changes away (position and name stay).
    const auto reverted = must(updateInstance(scene, root, after, after, false));
    CHECK(scene.get<EntityInfo>(reverted.root).name == "Big Coin");
    CHECK(need(scene.tryGet<Transform>(reverted.root)).position == Vec2{100, 40});
    CHECK(need(scene.tryGet<Sprite>(reverted.root)).size == Vec2{12, 12});
    CHECK(scene.tryGet<KinematicBody>(reverted.root) == nullptr);
    CHECK(scene.tryGet<Tags>(reverted.root) != nullptr);
    CHECK(child(scene, reverted.root, "Sparkle").isNull());
}

TEST_CASE("Prefabs: deleted entities stay deleted; entities the prefab drops are dropped") {
    const Scene before = coinPrefab();
    Scene scene;
    const Entity a = must(instantiatePrefab(scene, before, kPrefabId, Guid::generate()));
    const Entity b = must(instantiatePrefab(scene, before, kPrefabId, Guid::generate()));
    scene.destroyEntity(child(scene, a, "Glow")); // deleted in instance a
    const Entity watcher = scene.createEntity("Watcher");
    scene.set(watcher, Camera{idOf(scene, child(scene, b, "Glow")), 1.0f, true, {}});

    // The prefab drops Glow.
    Scene after = before;
    after.destroyEntity(after.entities()[1]);
    const Entity newA = must(updateInstance(scene, a, before, after)).root;
    const Entity newB = must(updateInstance(scene, scene.findById(idOf(scene, b)), before, after)).root;
    CHECK(scene.children(newA).empty());
    CHECK(scene.children(newB).empty());
    CHECK(need(scene.tryGet<Camera>(watcher)).follow.isNil()); // the reference to the removed child is cleared

    // A child deleted in an instance does not come back when the prefab changes otherwise.
    Scene c = before;
    const Entity inst = must(instantiatePrefab(scene, c, kPrefabId, Guid::generate()));
    scene.destroyEntity(child(scene, inst, "Glow"));
    Scene c2 = c;
    need(c2.tryGet<Sprite>(c2.entities().front())).tint = Color{0, 0, 255, 255};
    const Entity updated = must(updateInstance(scene, inst, c, c2)).root;
    CHECK(scene.children(updated).empty());
    CHECK(need(scene.tryGet<Sprite>(updated)).tint == Color{0, 0, 255, 255});
}

TEST_CASE("Prefabs: script property values set on an instance survive updates") {
    const assets::AssetId script{Guid(0x5c000000u, 7)};
    Scene before = coinPrefab();
    before.set(before.entities().front(), Scripts{{AttachedScript{script, {{"points", 10.0}}}}});
    Scene scene;
    const Entity coin = must(instantiatePrefab(scene, before, kPrefabId, Guid::generate()));
    need(scene.tryGet<Scripts>(coin)).items[0].properties = {{"points", 50.0}, {"label", std::string("big")}};

    Scene after = before;
    need(after.tryGet<Scripts>(after.entities().front())).items[0].properties = {{"points", 20.0}, {"speed", 2.0}};
    const Entity root = must(updateInstance(scene, coin, before, after)).root;
    const auto& props = need(scene.tryGet<Scripts>(root)).items.at(0).properties;
    const auto value = [&](std::string_view name) -> const ScriptValue* {
        for (const auto& p : props) {
            if (p.name == name) return &p.value;
        }
        return nullptr;
    };
    REQUIRE(value("points") != nullptr);
    CHECK(std::get<double>(*value("points")) == 50.0); // this instance's value
    REQUIRE(value("label") != nullptr);                 // set only here
    REQUIRE(value("speed") != nullptr);                 // new in the prefab
    CHECK(std::get<double>(*value("speed")) == 2.0);
}

TEST_CASE("Prefabs: a nested instance stays consistent inside another prefab's instances") {
    // Prefab B = coinPrefab; prefab A = a "Row" holding one instance of B.
    const Scene b = coinPrefab();
    const assets::AssetId bId = kPrefabId;
    Scene authoring;
    const Entity row = authoring.createEntity("Row");
    authoring.set(row, Transform{});
    const Entity inner = must(instantiatePrefab(authoring, b, bId, Guid::generate(), row));
    need(authoring.tryGet<Transform>(inner)).position = {16, 0};
    const Scene a = prefabFromEntities(authoring, row, Guid::generate());

    Scene scene;
    const Entity rowInstance = must(instantiatePrefab(scene, a, assets::AssetId{Guid(0x9f000000u, 2)}, Guid::generate()));
    const auto coins = findInstances(scene, bId);
    REQUIRE(coins.size() == 1);
    CHECK(scene.parent(coins[0]) == rowInstance);

    // Updating the inner instance of B maps its entities (ids compose).
    Scene b2 = b;
    need(b2.tryGet<Sprite>(b2.entities().front())).tint = Color{1, 2, 3, 255};
    const auto update = must(updateInstance(scene, coins[0], b, b2));
    CHECK(update.keptEntities == 0);
    CHECK(scene.children(update.root).size() == 1); // Glow was recognised, not duplicated
    CHECK(need(scene.tryGet<Sprite>(update.root)).tint == Color{1, 2, 3, 255});
    CHECK(need(scene.tryGet<Transform>(update.root)).position == Vec2{16, 0});
}

// --- Spawning --------------------------------------------------------------------------------

namespace {

struct SpawnGame {
    ProjectManifest project;
    std::shared_ptr<scripting::ScriptLibrary> library = std::make_shared<scripting::ScriptLibrary>();
    std::shared_ptr<PrefabLibrary> prefabs = std::make_shared<PrefabLibrary>();
    Scene scene;

    SpawnGame() {
        project.name = "Spawn";
        project.input.actions = {{"jump", {Key::Space}}};
    }
    assets::AssetId addScript(std::uint64_t n, std::string_view source) {
        const assets::AssetId id{Guid(0x5c000000u, n)};
        must(library->add(id, "script" + std::to_string(n), source));
        return id;
    }
    void addPrefab(const std::string& name, Scene content, std::uint64_t n = 1) {
        prefabs->add(Prefab{assets::AssetId{Guid(0x9f000000u, n)}, name, std::move(content)});
    }
    std::unique_ptr<runtime::GameSession> start(std::uint64_t seed = 1) {
        runtime::SessionOptions options;
        options.prefabs = prefabs;
        options.seed = seed;
        return must(runtime::GameSession::create(project, scene, library, options));
    }
};

class Logs {
public:
    Logs() {
        m_previous = log::minLevel();
        log::setMinLevel(log::Level::Info);
        log::setConsoleOutput(false);
        m_handle = log::addSink([this](log::Level, std::string_view m) { lines.emplace_back(m); });
    }
    ~Logs() {
        log::removeSink(m_handle);
        log::setConsoleOutput(true);
        log::setMinLevel(m_previous);
    }
    Logs(const Logs&) = delete;
    Logs& operator=(const Logs&) = delete;
    [[nodiscard]] bool has(std::string_view text) const {
        for (const auto& l : lines) {
            if (l.find(text) != std::string::npos) return true;
        }
        return false;
    }
    std::vector<std::string> lines;

private:
    int m_handle = 0;
    log::Level m_previous = log::Level::Info;
};

} // namespace

TEST_CASE("Spawning: World.spawn creates prefab copies whose scripts start next tick") {
    Logs logs;
    SpawnGame game;
    const auto bulletScript = game.addScript(1, R"(
        local Bullet = { properties = { speed = 60 } }
        function Bullet:Start() print("bullet start " .. self.entity.name) end
        function Bullet:Tick(dt)
            self.entity.position += Vec2(self.speed * dt, 0)
        end
        return Bullet)");
    Scene bullet;
    const Entity b = bullet.createEntity("Bullet");
    bullet.set(b, Transform{});
    bullet.set(b, Scripts{{AttachedScript{bulletScript, {}}}});
    const Entity trail = bullet.createEntity("Trail", b);
    bullet.set(trail, Transform{{-4, 0}});
    game.addPrefab("Bullet", std::move(bullet));

    const auto gunScript = game.addScript(2, R"(
        local Gun = {}
        function Gun:Start()
            local shot = World.spawn("Bullet", Vec2(10, 20))
            print("spawned " .. shot.name .. " at " .. shot.position.x .. "," .. shot.position.y
                  .. " with " .. #shot:getChildren() .. " child")
            local ok, err = pcall(World.spawn, "Bulet")
            print(err)
            local child = World.spawn("Bullet", nil, self.entity)
            print("child of " .. child.parent.name .. " at " .. child.position.x)
        end
        return Gun)");
    const Entity gun = game.scene.createEntity("Gun");
    game.scene.set(gun, Transform{{100, 100}});
    game.scene.set(gun, Scripts{{AttachedScript{gunScript, {}}}});

    auto session = game.start();
    session->tick({});
    CHECK(logs.has("spawned Bullet at 10,20 with 1 child"));
    CHECK(logs.has("no prefab named 'Bulet' (did you mean 'Bullet'?)"));
    CHECK(logs.has("child of Gun at 100"));
    CHECK_FALSE(logs.has("bullet start")); // not yet: next tick
    session->tick({});
    CHECK(logs.has("bullet start Bullet"));
    const auto spawned = findInstances(session->scene(), assets::AssetId{Guid(0x9f000000u, 1)});
    REQUIRE(spawned.size() == 2); // in hierarchy order: the Gun's child, then the root one
    CHECK(session->scene().parent(spawned[0]) == gun);
    CHECK(session->scene().worldPosition(spawned[0]).x == doctest::Approx(101.0)); // moved one tick at 60/s
    CHECK(session->scene().worldPosition(spawned[1]).x == doctest::Approx(11.0));
    CHECK(session->state().scriptErrors == 0);

    // Ids are deterministic: a second run spawns the same ids.
    auto again = game.start();
    again->tick({});
    const auto spawnedAgain = findInstances(again->scene(), assets::AssetId{Guid(0x9f000000u, 1)});
    REQUIRE(spawnedAgain.size() == 2);
    CHECK(idOf(again->scene(), spawnedAgain[0]) == idOf(session->scene(), spawned[0]));
}

TEST_CASE("Spawning: limits per tick, unknown prefabs and client realm") {
    Logs logs;
    SpawnGame game;
    Scene dot;
    dot.set(dot.createEntity("Dot"), Transform{});
    game.addPrefab("Dot", std::move(dot));
    const auto flood = game.addScript(1, R"(
        local Flood = {}
        function Flood:Tick()
            local ok, err = pcall(function()
                for i = 1, 5000 do World.spawn("Dot", Vec2(i, 0)) end
            end)
            if not ok then print(err) end
        end
        return Flood)");
    const Entity e = game.scene.createEntity("Flood");
    game.scene.set(e, Scripts{{AttachedScript{flood, {}}}});
    auto session = game.start();
    session->tick({});
    CHECK(logs.has("too many entities spawned in one tick (the limit is 1000)"));
    CHECK(session->scene().entityCount() == 1 + runtime::kMaxSpawnedEntitiesPerTick);
    session->tick({}); // the limit is per tick
    CHECK(session->scene().entityCount() == 1 + 2 * runtime::kMaxSpawnedEntitiesPerTick);

    // Without prefabs.
    SpawnGame empty;
    const auto lonely = empty.addScript(1, R"(
        local L = {}
        function L:Start() print(select(2, pcall(World.spawn, "X"))) end
        return L)");
    empty.scene.set(empty.scene.createEntity("L"), Scripts{{AttachedScript{lonely, {}}}});
    auto s2 = empty.start();
    s2->tick({});
    CHECK(logs.has("this game has no prefabs"));
}

TEST_CASE("Spawning: a spawned tilemap is solid ground; destroying it removes the ground") {
    SpawnGame game;
    const auto crumble = game.addScript(1, R"(
        local Crumble = {}
        function Crumble:Tick()
            if Game.tickCount() >= 90 then self.entity:destroy() end
        end
        return Crumble)");
    Scene floor;
    const Entity f = floor.createEntity("Floor");
    floor.set(f, Transform{});
    Tilemap map;
    map.tileset = kTexture;
    map.width = 4;
    map.height = 1;
    map.tiles = {0, 0, 0, 0};
    floor.set(f, map);
    floor.set(f, Scripts{{AttachedScript{crumble, {}}}});
    game.addPrefab("Floor", std::move(floor));
    const Entity body = game.scene.createEntity("Body");
    game.scene.set(body, Transform{{30, 0}});
    game.scene.set(body, BoxCollider{{8, 8}, {}, false});
    game.scene.set(body, KinematicBody{});
    auto session = game.start();
    CHECK(must(session->spawn("Floor", {0, 32})).index != 0xffffffffu);
    const auto fall = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            need(session->scene().tryGet<KinematicBody>(body)).velocity = {0, 40};
            session->tick({});
        }
    };
    fall(80);
    CHECK(session->scene().worldPosition(body).y == doctest::Approx(28.0)); // resting on the tiles at y = 32
    fall(40);
    CHECK(session->scene().worldPosition(body).y > 30.0f); // the floor is gone
    CHECK(session->state().scriptErrors == 0);
}

// --- Studio workflow ---------------------------------------------------------------------

TEST_CASE("Editor prefabs: save as prefab, place, apply to every copy, revert, undo") {
    TempFolder temp;
    auto project = must(editor::EditorProject::create(temp.path() / "game", "Prefab Game"));
    const assets::AssetId sceneId = project->manifest().startScene;
    const assets::AssetId level2 = must(project->createScene("Level 2"));

    editor::SceneDocument doc(sceneId, must(project->loadScene(sceneId)));
    const Entity crate = doc.createEntity(editor::EntityPreset::SolidBlock, "Crate", kNullEntity, {40, 40});
    const Entity label = doc.createEntity(editor::EntityPreset::Empty, "Label", crate, {40, 30});
    (void)label;
    const auto prefabId = must(editor::createPrefabFromEntity(*project, doc, crate, "Crate"));
    CHECK(project->asset(prefabId) != nullptr);
    CHECK(std::filesystem::exists(project->absolutePath(prefabId)));
    CHECK(need(doc.scene().tryGet<PrefabInstance>(crate)).prefab == prefabId);
    CHECK(project->asset(prefabId)->path == "prefabs/crate.prefab.json");

    // Place two more copies; give one its own collider offset.
    const Entity copy1 = must(editor::placePrefab(*project, doc, prefabId, {100, 40}));
    const Entity copy2 = must(editor::placePrefab(*project, doc, prefabId, {160, 40}));
    CHECK(doc.scene().get<EntityInfo>(copy1).name == "Crate 2");
    CHECK(doc.scene().worldPosition(copy1) == Vec2{100, 40});
    CHECK(doc.scene().children(copy1).size() == 1);
    need(doc.sceneForEdit().tryGet<BoxCollider>(copy2)).offset = {2, 2};

    // A copy in another scene file.
    {
        editor::SceneDocument other(level2, must(project->loadScene(level2)));
        (void)must(editor::placePrefab(*project, other, prefabId, {10, 10}));
        must(other.save(*project));
    }

    // Change the original and apply: the copies follow, keeping their own changes.
    const auto setSize = [&](Vec2 size) {
        doc.beginEdit("size");
        need(doc.sceneForEdit().tryGet<BoxCollider>(doc.scene().findByName("Crate"))).size = size;
        doc.markChanged();
        doc.endEdit();
    };
    setSize({20, 20});
    const auto report = must(editor::applyToPrefab(*project, doc, crate));
    CHECK(report.instancesUpdated == 3);
    REQUIRE(report.filesUpdated.size() == 1);
    CHECK(report.filesUpdated[0] == "Level 2");
    const Scene& s = doc.scene();
    const Entity c1 = s.findByName("Crate 2");
    const Entity c2 = s.findByName("Crate 3");
    CHECK(need(s.tryGet<BoxCollider>(c1)).size == Vec2{20, 20});
    CHECK(need(s.tryGet<BoxCollider>(c2)).size == Vec2{20, 20});
    CHECK(need(s.tryGet<BoxCollider>(c2)).offset == Vec2{2, 2}); // its own change
    CHECK(s.worldPosition(c1) == Vec2{100, 40});
    {
        const Scene other = must(project->loadScene(level2));
        const auto copies = findInstances(other, prefabId);
        REQUIRE(copies.size() == 1);
        CHECK(need(other.tryGet<BoxCollider>(copies[0])).size == Vec2{20, 20});
    }
    // The prefab file has the new content with the root at [0, 0].
    const Prefab saved = must(project->loadPrefab(prefabId));
    CHECK(need(saved.scene.tryGet<Transform>(saved.root())).position == Vec2{0, 0});
    CHECK(need(saved.scene.tryGet<BoxCollider>(saved.root())).size == Vec2{20, 20});

    // Undo in the open scene restores the copies there (the prefab file stays).
    doc.undo();
    CHECK(need(doc.scene().tryGet<BoxCollider>(doc.scene().findByName("Crate 2"))).size == Vec2{16, 16});
    doc.redo();

    // Revert throws away a copy's own changes.
    const Entity reverted = must(editor::revertToPrefab(*project, doc, doc.scene().findByName("Crate 3")));
    CHECK(need(doc.scene().tryGet<BoxCollider>(reverted)).offset == Vec2{0, 0});
    CHECK(doc.scene().worldPosition(reverted) == Vec2{160, 40});

    // Duplicated and pasted copies are copies too, and update like the others.
    doc.select(reverted);
    const auto dup = doc.duplicateSelection();
    REQUIRE(dup.size() == 1);
    CHECK(need(doc.scene().tryGet<PrefabInstance>(dup[0])).prefab == prefabId);
    const auto copied = doc.copySelection();
    const auto pasted = must(doc.paste(copied, &project->assets()));
    REQUIRE(pasted.size() == 1);
    setSize({24, 24});
    const auto again = must(editor::applyToPrefab(*project, doc, doc.scene().findByName("Crate")));
    CHECK(again.instancesUpdated == 5); // Crate 2, Crate 3, the duplicate, the pasted copy, Level 2's
    for (const Entity e : findInstances(doc.scene(), prefabId)) {
        CHECK(doc.scene().children(e).size() == 1); // children recognised, never doubled
        CHECK(need(doc.scene().tryGet<BoxCollider>(e)).size == Vec2{24, 24});
    }
    must(doc.save(*project));

    // Prefab assets are in use: deleting is refused while copies exist; uses include prefab contents.
    CHECK_FALSE(project->deleteAsset(prefabId, {{sceneId, &doc.scene()}}).ok());

    // The game loads and plays with the prefab, and a script can spawn it.
    DirectoryFileSystem files(temp.path() / "game");
    const auto game = must(runtime::loadGame(files));
    REQUIRE(game.prefabs != nullptr);
    CHECK(game.prefabs->findByName("Crate") != nullptr);
    auto session = must(runtime::startSession(game));
    CHECK(session->spawn("Crate", {0, 0}).ok());
}

TEST_CASE("Editor prefabs: a prefab cannot contain itself, and broken prefabs are reported") {
    TempFolder temp;
    auto project = must(editor::EditorProject::create(temp.path() / "game", "Prefab Game"));
    const assets::AssetId sceneId = project->manifest().startScene;
    editor::SceneDocument doc(sceneId, must(project->loadScene(sceneId)));
    const Entity box = doc.createEntity(editor::EntityPreset::Empty, "Box");
    const auto prefabId = must(editor::createPrefabFromEntity(*project, doc, box, "Box"));
    const Entity inner = must(editor::placePrefab(*project, doc, prefabId, {0, 0}, box));
    (void)inner;
    auto applied = editor::applyToPrefab(*project, doc, box);
    REQUIRE_FALSE(applied.ok());
    CHECK(contains(applied.error().message, "cannot contain itself"));

    // A broken prefab file stops the game from loading, with the file named.
    {
        std::ofstream out(project->absolutePath(prefabId), std::ios::binary);
        out << R"({"format": "px.prefab", "formatVersion": 1, "sceneFormatVersion": 3, "entities": [{"id": 5}]})";
    }
    DirectoryFileSystem files(temp.path() / "game");
    auto game = runtime::loadGame(files);
    REQUIRE_FALSE(game.ok());
    CHECK(contains(game.error().message, "box.prefab.json"));
}
