// Tilesets: the file format, tiles that are one-way floors, slopes or not
// blocking in a running game, tile tags for scripts, and Studio's tile tools.

#include <doctest.h>

#include "px/core/FileSystem.hpp"
#include "px/core/Log.hpp"
#include "px/editor/EditorProject.hpp"
#include "px/editor/SceneDocument.hpp"
#include "px/project/Tileset.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/runtime/GameSession.hpp"
#include "px/scripting/ScriptLibrary.hpp"

#include <filesystem>
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
template <class T>
T& need(T* component) {
    if (component == nullptr) throw std::runtime_error("expected component is missing");
    return *component;
}
bool contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

const assets::AssetId kTiles{Guid(0x7e000000u, 1)};

// Tiles: 0 solid ground, 1 one-way plank, 2 slope '/', 3 not blocking, 4 water (solid, tagged).
Tileset makeTileset() {
    Tileset t;
    t.texture = kTiles;
    t.set(1, {TileCollision::OneWay, {}});
    t.set(2, {TileCollision::SlopeUpRight, {}});
    t.set(3, {TileCollision::None, {}});
    t.set(4, {TileCollision::Solid, {"water", "deep"}});
    return t;
}

// A platformer level (16 x 8 tiles of 16 units):
//   row 7: ground; a slope at column 9 rises onto a step at columns 10-15 (row 6);
//   row 5: a one-way plank at columns 2-4; column 6 rows 4-6: a "wall" of tile 3
//   that does not block.
struct Level {
    ProjectManifest project;
    std::shared_ptr<scripting::ScriptLibrary> scripts = std::make_shared<scripting::ScriptLibrary>();
    std::shared_ptr<TilesetLibrary> tilesets = std::make_shared<TilesetLibrary>();
    Scene scene;
    Entity hero;

    Level() {
        project.name = "Tiles";
        project.input.actions = {{"move_left", {Key::Left}}, {"move_right", {Key::Right}}, {"jump", {Key::Space}}};
        tilesets->add({assets::AssetId{Guid(0x7f000000u, 1)}, "Tiles", makeTileset()});
        const Entity map = scene.createEntity("Level");
        scene.set(map, Transform{});
        Tilemap tm;
        tm.tileset = kTiles;
        tm.width = 16;
        tm.height = 8;
        tm.tiles.assign(16 * 8, -1);
        const auto set = [&](int x, int y, int t) { tm.tiles[static_cast<std::size_t>(y * 16 + x)] = t; };
        for (int x = 0; x < 16; ++x) set(x, 7, 0);
        set(9, 6, 2);
        for (int x = 10; x < 16; ++x) set(x, 6, 0);
        for (int x = 2; x <= 4; ++x) set(x, 5, 1);
        for (int y = 4; y <= 6; ++y) set(6, y, 3);
        set(0, 6, 4);
        scene.set(map, tm);
        hero = scene.createEntity("Hero");
        scene.set(hero, Transform{{56, 100}});
        scene.set(hero, BoxCollider{{10, 14}, {}, false});
        scene.set(hero, KinematicBody{});
        scene.set(hero, PlatformerController{});
        scene.set(hero, Player{});
    }
    std::unique_ptr<runtime::GameSession> start(bool withTilesets = true) {
        runtime::SessionOptions options;
        if (withTilesets) options.tilesets = tilesets;
        return must(runtime::GameSession::create(project, scene, scripts, options));
    }
};

runtime::InputFrame keys(const runtime::GameSession& s, std::initializer_list<std::string_view> names, bool press) {
    std::uint32_t bits = 0;
    for (auto n : names) bits |= 1u << s.actions().find(n);
    return {bits, press ? bits : 0u};
}

float bottomOf(const runtime::GameSession& s, Entity e) {
    return s.scene().worldPosition(e).y + s.scene().get<BoxCollider>(e).size.y * 0.5f;
}

} // namespace

TEST_CASE("Tilesets: files round-trip and refuse bad data") {
    const Tileset t = makeTileset();
    const std::string text = serializeTileset(t);
    const Tileset back = must(parseTileset(text, "tiles.tileset.json"));
    CHECK(serializeTileset(back) == text);
    CHECK(back.info(1).collision == TileCollision::OneWay);
    CHECK(back.info(4).hasTag("deep"));
    CHECK(back.info(99).isDefault());

    const auto error = [](const std::string& tiles) {
        const std::string json = R"({"format": "px.tileset", "formatVersion": 1, "texture": ")" + kTiles.toString() +
                                 R"(", "tiles": )" + tiles + "}";
        auto r = parseTileset(json, "bad.tileset.json");
        return r ? std::string() : r.error().message;
    };
    CHECK(contains(error(R"([{"tile": 1, "collision": "bouncy"}])"), "unknown collision 'bouncy'"));
    CHECK(contains(error(R"([{"tile": 1}, {"tile": 1}])"), "listed twice"));
    CHECK(contains(error(R"([{"tile": -1}])"), "0 to 65535"));
    CHECK(contains(error(R"([{"tile": 1, "tags": [""]}])"), "1 to 32 characters"));
    CHECK(contains(error(R"([{"tile": 1, "colision": "none"}])"), "unknown field 'colision'"));
    // Setting a tile back to the default removes it.
    Tileset u = makeTileset();
    u.set(3, {});
    CHECK_FALSE(u.tiles.contains(3));
}

TEST_CASE("Tilesets: one-way floors, slopes and tiles that do not block, in a running game") {
    Level level;
    auto s = level.start();
    for (int i = 0; i < 60; ++i) s->tick({});
    CHECK(bottomOf(*s, level.hero) == doctest::Approx(112.0)); // on the ground (row 7)

    // Walk right through the non-blocking column, up the slope onto the step.
    for (int i = 0; i < 90; ++i) s->tick(keys(*s, {"move_right"}, i == 0));
    CHECK(s->scene().worldPosition(level.hero).x > 160.0f);
    CHECK(bottomOf(*s, level.hero) == doctest::Approx(96.0).epsilon(0.01)); // on the step (row 6)
    CHECK(s->scene().get<KinematicBody>(level.hero).onGround);

    // Without the tileset, column 6 is a wall.
    auto plain = level.start(false);
    for (int i = 0; i < 90; ++i) plain->tick(keys(*plain, {"move_right"}, i == 0));
    CHECK(plain->scene().worldPosition(level.hero).x < 96.0f);
}

TEST_CASE("Tilesets: jumping up through a one-way plank and landing on it") {
    Level level;
    level.scene.get<Transform>(level.hero).position = {56, 100}; // under the plank (columns 2-4, top y = 80)
    auto s = level.start();
    for (int i = 0; i < 30; ++i) s->tick({});
    s->tick(keys(*s, {"jump"}, true));
    float lowest = 1000.0f;
    for (int i = 0; i < 60; ++i) {
        s->tick(keys(*s, {"jump"}, false));
        lowest = std::min(lowest, bottomOf(*s, level.hero));
    }
    CHECK(lowest < 80.0f);                                      // went up through the plank
    CHECK(bottomOf(*s, level.hero) == doctest::Approx(80.0f)); // and came to rest on it
    CHECK(s->scene().get<KinematicBody>(level.hero).onGround);
}

TEST_CASE("Tilesets: one-way boxes (platforms)") {
    Level level;
    const Entity platform = level.scene.createEntity("Platform");
    level.scene.set(platform, Transform{{56, 80}});
    BoxCollider c{{48, 8}, {}, false};
    c.oneWay = true;
    level.scene.set(platform, c);
    auto s = level.start();
    for (int i = 0; i < 30; ++i) s->tick({});
    s->tick(keys(*s, {"jump"}, true));
    for (int i = 0; i < 60; ++i) s->tick(keys(*s, {"jump"}, false));
    CHECK(bottomOf(*s, level.hero) == doctest::Approx(76.0f)); // on top of the platform
}

TEST_CASE("Tilesets: scripts read tiles and their tags") {
    Level level;
    const assets::AssetId script{Guid(0x5c000000u, 1)};
    REQUIRE(level.scripts->add(script, "probe", R"(
        local P = {}
        function P:Start()
            print("ground " .. tostring(World.tileAt(Vec2(40, 120))))
            print("air " .. tostring(World.tileAt(Vec2(40, 10))))
            print("water " .. tostring(World.tileHasTag(Vec2(8, 104), "water")))
            print("dry " .. tostring(World.tileHasTag(Vec2(40, 120), "water")))
        end
        return P)").ok());
    const Entity probe = level.scene.createEntity("Probe");
    level.scene.set(probe, Scripts{{AttachedScript{script, {}}}});
    std::vector<std::string> lines;
    const auto previous = log::minLevel();
    log::setMinLevel(log::Level::Info);
    log::setConsoleOutput(false);
    const int sink = log::addSink([&](log::Level, std::string_view m) { lines.emplace_back(m); });
    auto s = level.start();
    s->tick({});
    log::removeSink(sink);
    log::setConsoleOutput(true);
    log::setMinLevel(previous);
    const auto has = [&](std::string_view text) {
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return contains(l, text); });
    };
    CHECK(has("ground 0"));
    CHECK(has("air nil"));
    CHECK(has("water true"));
    CHECK(has("dry false"));
}

TEST_CASE("Tilesets: Studio's tile tools and tile properties") {
    const auto folder = std::filesystem::temp_directory_path() / ("px-tiles-test-" + Guid::generate().toString());
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{folder};
    auto project = must(editor::EditorProject::create(folder / "game", "Tile Game",
                                                      {project::GameType::Platformer, std::filesystem::path(PX_EXAMPLES_DIR) / "platformer"}));
    const assets::AssetId sceneId = project->manifest().startScene;
    editor::SceneDocument doc(sceneId, must(project->loadScene(sceneId)));
    const Entity map = doc.scene().findByName("Level");
    REQUIRE_FALSE(map.isNull());
    const Tilemap before = doc.scene().get<Tilemap>(map);

    // Rectangle fill and bucket fill, each one undo step.
    CHECK(doc.fillTiles(map, 0, 0, 2, 1, 3) == 6);
    CHECK(doc.scene().get<Tilemap>(map).tileAt(2, 1) == 3);
    CHECK(doc.fillTiles(map, 2, 1, 0, 0, 3) == 0); // nothing to change
    const int filled = doc.floodFillTiles(map, 1, 1, 7);
    CHECK(filled == 6);
    CHECK(doc.scene().get<Tilemap>(map).tileAt(0, 0) == 7);
    doc.undo();
    CHECK(doc.scene().get<Tilemap>(map).tileAt(0, 0) == 3);
    doc.undo();
    CHECK(doc.scene().get<Tilemap>(map).tiles == before.tiles);
    // A bucket fill of empty space fills the whole connected area of empty cells.
    const int empty = doc.floodFillTiles(map, 0, 0, 1);
    CHECK(empty > 20);
    doc.undo();

    // The template's image already has its tileset; the editor finds it, and creates one for other images.
    const assets::AssetId existing = project->tilesetFor(before.tileset, before.tileSize);
    REQUIRE_FALSE(existing.isNil());
    CHECK(must(project->createTileset(before.tileset, before.tileSize)) == existing);
    Tileset tiles = must(project->loadTileset(existing));
    CHECK(tiles.info(10).collision == TileCollision::OneWay);
    tiles.set(5, {TileCollision::None, {"grass"}});
    REQUIRE(project->saveTileset(existing, tiles).ok());
    CHECK(must(project->loadTileset(existing)).info(5).hasTag("grass"));
    assets::AssetId coin;
    for (const auto& a : project->assets().entries()) {
        if (a.name == "Coin" && a.type == assets::AssetType::Texture) coin = a.id;
    }
    const assets::AssetId made = must(project->createTileset(coin, {16, 16}));
    CHECK(project->asset(made)->path == "tilesets/coin.tileset.json");
    // An image used by a tileset cannot be deleted.
    CHECK_FALSE(project->deleteAsset(coin, {{sceneId, &doc.scene()}}).ok());
}
