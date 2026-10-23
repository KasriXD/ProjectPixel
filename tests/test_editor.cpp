#include <doctest.h>

#include "px/core/FileSystem.hpp"
#include "px/core/Log.hpp"
#include "px/editor/EditorProject.hpp"
#include "px/editor/Library.hpp"
#include "px/editor/LogBuffer.hpp"
#include "px/editor/Picking.hpp"
#include "px/editor/PlayMode.hpp"
#include "px/editor/SceneDocument.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/scene/ComponentInfo.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace px;
using namespace px::editor;
using namespace px::scene;

namespace {

const std::filesystem::path kExample = std::filesystem::path(PX_EXAMPLES_DIR) / "platformer";
const std::filesystem::path kLibrary = std::filesystem::path(PX_LIBRARY_DIR);

// A fresh folder for one test, removed afterwards.
class TempFolder {
public:
    TempFolder() {
        const auto base = std::filesystem::temp_directory_path();
        m_path = base / ("px-editor-test-" + Guid::generate().toString());
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

template <class T>
T must(Result<T> result) {
    if (!result) throw std::runtime_error(result.error().message);
    return std::move(result.value());
}
void must(const Status& status) {
    if (!status) throw std::runtime_error(status.error().message);
}

bool contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

std::vector<std::string> names(const Scene& scene, const std::vector<Entity>& entities) {
    std::vector<std::string> out;
    for (const Entity e : entities) out.push_back(scene.get<EntityInfo>(e).name);
    return out;
}

void writeFile(const std::filesystem::path& path, std::string_view content) {
    std::ofstream out(path, std::ios::binary);
    out << content;
}

} // namespace

TEST_CASE("Editor: a new empty project is valid, opens again and plays") {
    TempFolder temp;
    const auto folder = temp.path() / "My Game";
    auto project = must(EditorProject::create(folder, "My Game"));
    CHECK(project->manifest().name == "My Game");
    CHECK_FALSE(project->manifest().startScene.isNil());
    CHECK(std::filesystem::exists(folder / "project.json"));

    auto reopened = must(EditorProject::open(folder));
    CHECK(reopened->manifest().id == project->manifest().id);
    DirectoryFileSystem files(folder);
    auto game = must(runtime::loadGame(files));
    CHECK(game.startScene.findByName("Main Camera") != kNullEntity);
    // A player who falls off the level respawns (kill plane below the screen).
    CHECK(game.startScene.settings().killPlaneY.value_or(0.0f) > 180.0f);
    CHECK(runtime::startSession(game).ok());

    // Creating into a folder that is not empty is refused.
    auto again = EditorProject::create(folder, "Again");
    REQUIRE_FALSE(again.ok());
    CHECK(contains(again.error().message, "is not empty"));
    CHECK_FALSE(EditorProject::create(temp.path() / "x", "").ok());
    CHECK_FALSE(EditorProject::open(temp.path() / "nothing-here").ok());
}

TEST_CASE("Editor: a project from the platformer template gets its own id and name") {
    TempFolder temp;
    auto project = must(EditorProject::create(temp.path() / "copy", "Copy", {.templateFolder = kExample}));
    auto original = must(EditorProject::open(kExample));
    CHECK(project->manifest().name == "Copy");
    CHECK(project->manifest().id != original->manifest().id);
    CHECK(project->assets().size() == original->assets().size());
    auto scene = must(project->loadScene(project->manifest().startScene));
    CHECK(scene.findByName("Player") != kNullEntity);
}

TEST_CASE("Editor: undo and redo restore the scene, selection and dirty state") {
    SceneDocument doc(assets::AssetId{Guid(1, 1)}, Scene{});
    CHECK_FALSE(doc.isDirty());
    const Entity a = doc.createEntity(EntityPreset::Empty, "A", kNullEntity, {10, 20});
    CHECK(doc.isDirty());
    CHECK(doc.isSelected(a));
    CHECK(doc.scene().get<Transform>(a).position == Vec2{10, 20});
    doc.rename(a, "Renamed");
    CHECK(doc.undoSteps() == 2);
    CHECK(doc.undoLabel() == "Rename");

    doc.undo();
    CHECK(doc.scene().get<EntityInfo>(doc.scene().entities()[0]).name == "A");
    doc.undo();
    CHECK(doc.scene().entityCount() == 0);
    CHECK(doc.selection().empty());
    CHECK_FALSE(doc.isDirty()); // back to the state it was saved/opened in
    CHECK(doc.canRedo());
    doc.redo();
    doc.redo();
    REQUIRE(doc.scene().entityCount() == 1);
    CHECK(doc.scene().get<EntityInfo>(doc.scene().entities()[0]).name == "Renamed");
    CHECK(doc.selection().size() == 1); // selection came back with the entity

    // A new edit clears redo.
    doc.undo();
    doc.createEntity(EntityPreset::Group, "G");
    CHECK_FALSE(doc.canRedo());

    // No-op edits create no undo step.
    const std::size_t steps = doc.undoSteps();
    doc.rename(doc.scene().findByName("G"), "G");
    doc.beginEdit("Nothing");
    doc.endEdit();
    CHECK(doc.undoSteps() == steps);
}

TEST_CASE("Editor: a drag is one undo step and can be cancelled") {
    SceneDocument doc(assets::AssetId{Guid(1, 1)}, Scene{});
    const Entity parent = doc.createEntity(EntityPreset::Empty, "Parent", kNullEntity, {100, 0});
    const Entity child = doc.createEntity(EntityPreset::Empty, "Child", parent, {110, 0});
    CHECK(doc.scene().get<Transform>(child).position == Vec2{10, 0}); // created at a world position
    doc.setSelection({parent, child});
    const std::size_t steps = doc.undoSteps();

    doc.beginEdit("Move");
    for (int i = 0; i < 10; ++i) doc.translateSelection({1, 0}); // child moves with its parent only once
    doc.endEdit();
    CHECK(doc.undoSteps() == steps + 1);
    CHECK(doc.scene().worldPosition(parent) == Vec2{110, 0});
    CHECK(doc.scene().worldPosition(child) == Vec2{120, 0});

    doc.beginEdit("Move");
    doc.translateSelection({50, 50});
    doc.cancelEdit();
    CHECK(doc.scene().worldPosition(parent) == Vec2{110, 0});
    CHECK(doc.undoSteps() == steps + 1);

    doc.undo();
    CHECK(doc.scene().worldPosition(parent) == Vec2{100, 0});
}

TEST_CASE("Editor: delete, duplicate, copy and paste keep references sensible") {
    SceneDocument doc(assets::AssetId{Guid(1, 1)}, Scene{});
    const Entity player = doc.createEntity(EntityPreset::Player);
    const Entity camera = doc.createEntity(EntityPreset::Camera);
    CHECK(doc.scene().get<Camera>(camera).follow == doc.scene().get<EntityInfo>(player).id);

    const Entity group = doc.createEntity(EntityPreset::Group, "Coins");
    const Entity coin = doc.createEntity(EntityPreset::Trigger, "Coin 14", group);
    doc.select(coin);
    const auto copies = doc.duplicateSelection();
    REQUIRE(copies.size() == 1);
    CHECK(doc.scene().get<EntityInfo>(copies[0]).name == "Coin 15");
    CHECK(doc.scene().parent(copies[0]) == group);
    CHECK(names(doc.scene(), doc.scene().children(group)) == std::vector<std::string>{"Coin 14", "Coin 15"});

    // Duplicating a camera together with its target rewires the copy.
    doc.setSelection({player, camera});
    const auto pair = doc.duplicateSelection();
    REQUIRE(pair.size() == 2);
    CHECK(doc.scene().get<Camera>(pair[1]).follow == doc.scene().get<EntityInfo>(pair[0]).id);
    CHECK(doc.scene().get<EntityInfo>(pair[0]).name == "Player 2");

    // Copy and paste.
    doc.select(group);
    const std::string clip = doc.copySelection();
    auto pasted = doc.paste(clip, nullptr);
    REQUIRE(pasted.ok());
    REQUIRE(pasted->size() == 1);
    CHECK(doc.scene().get<EntityInfo>((*pasted)[0]).name == "Coins 2");
    CHECK(doc.scene().children((*pasted)[0]).size() == 2);
    CHECK_FALSE(doc.paste("not json", nullptr).ok());

    // Deleting the player clears the camera's reference, so the scene still saves.
    doc.select(player);
    doc.deleteSelection();
    CHECK(doc.scene().get<Camera>(camera).follow.isNil());
    CHECK(doc.problems(nullptr).empty());
}

TEST_CASE("Editor: components bring their requirements; removing a needed one is refused") {
    SceneDocument doc(assets::AssetId{Guid(1, 1)}, Scene{});
    const Entity e = doc.createEntity(EntityPreset::Group, "Hero");
    const ComponentInfo& controller = *findComponentType("PlatformerController");
    must(doc.addComponent(e, controller));
    CHECK(doc.scene().tryGet<Transform>(e) != nullptr);
    CHECK(doc.scene().tryGet<BoxCollider>(e) != nullptr);
    CHECK(doc.scene().tryGet<KinematicBody>(e) != nullptr);
    CHECK(doc.undoLabel() == "Add PlatformerController");
    CHECK_FALSE(doc.addComponent(e, controller).ok());

    const Status refused = doc.removeComponent(e, *findComponentType("KinematicBody"));
    REQUIRE_FALSE(refused.ok());
    CHECK(contains(refused.error().message, "needed by PlatformerController"));
    must(doc.removeComponent(e, controller));
    must(doc.removeComponent(e, *findComponentType("KinematicBody")));
    CHECK(doc.scene().tryGet<KinematicBody>(e) == nullptr);
    doc.undo();
    CHECK(doc.scene().tryGet<KinematicBody>(e) != nullptr);
}

TEST_CASE("Editor: tilemap painting and resizing") {
    SceneDocument doc(assets::AssetId{Guid(1, 1)}, Scene{});
    const Entity map = doc.createEntity(EntityPreset::Tilemap, "Ground", kNullEntity, {32, 16});
    CHECK(doc.scene().get<Tilemap>(map).width == 20);
    doc.beginEdit("Paint");
    CHECK(doc.setTile(map, 0, 0, 3));
    CHECK(doc.setTile(map, 1, 0, 3));
    CHECK_FALSE(doc.setTile(map, 1, 0, 3)); // unchanged
    CHECK_FALSE(doc.setTile(map, 99, 0, 3)); // outside
    doc.endEdit();
    CHECK(doc.undoLabel() == "Paint");
    CHECK(doc.scene().get<Tilemap>(map).tileAt(1, 0) == 3);

    const TileCoord cell = tileAt(doc.scene(), map, {32 + 16 * 1 + 3, 16 + 2});
    CHECK(cell.inside);
    CHECK(cell.x == 1);
    CHECK(cell.y == 0);
    CHECK_FALSE(tileAt(doc.scene(), map, {0, 0}).inside);

    doc.resizeTilemap(map, 2, 3);
    const auto& resized = doc.scene().get<Tilemap>(map);
    CHECK(resized.width == 2);
    CHECK(resized.height == 3);
    CHECK(resized.tiles == std::vector<int>{3, 3, -1, -1, -1, -1});
}

TEST_CASE("Editor: picking finds the topmost thing under the mouse") {
    Scene scene;
    const Entity ground = scene.createEntity("Ground");
    scene.set(ground, Transform{});
    Tilemap map;
    map.width = 4;
    map.height = 1;
    map.tiles = {0, -1, 0, 0};
    scene.set(ground, map);
    const Entity back = scene.createEntity("Back");
    scene.set(back, Transform{{8, 8}});
    Sprite low;
    low.layer = -5;
    scene.set(back, low);
    const Entity front = scene.createEntity("Front");
    scene.set(front, Transform{{10, 8}});
    Sprite high;
    high.layer = 5;
    scene.set(front, high);
    const Entity trigger = scene.createEntity("Trigger");
    scene.set(trigger, Transform{{40, 8}});
    scene.set(trigger, BoxCollider{{8, 8}, {}, true});
    const Entity marker = scene.createEntity("Marker");
    scene.set(marker, Transform{{100, 100}});
    scene.createEntity("Group"); // no Transform: never picked

    CHECK(editor::pick(scene, {9, 8}) == front);
    CHECK(pickAll(scene, {9, 8}) == std::vector<Entity>{front, back, ground});
    CHECK(editor::pick(scene, {40, 8}) == trigger);        // collider over tiles
    CHECK(editor::pick(scene, {20, 8}) == kNullEntity);    // empty tile, nothing else
    CHECK(editor::pick(scene, {50, 8}) == ground);         // a solid tile
    CHECK(editor::pick(scene, {101, 99}) == marker);       // handle of an entity with nothing visible
    CHECK(editor::pick(scene, {300, 300}) == kNullEntity);

    CHECK(entityBounds(scene, ground) == Rect{0, 0, 64, 16});
    const auto inside = entitiesInside(scene, Rect{30, 0, 20, 20});
    CHECK(inside == std::vector<Entity>{trigger});
    CHECK(snapToGrid({7.9f, -8.2f}, 16.0f) == Vec2{0, -16});
}

TEST_CASE("Editor: importing, renaming and deleting assets") {
    TempFolder temp;
    auto project = must(EditorProject::create(temp.path() / "p", "P"));
    const auto texture = must(project->importFile(kExample / "assets" / "coin.png"));
    const auto* entry = project->asset(texture);
    REQUIRE(entry != nullptr);
    CHECK(entry->type == assets::AssetType::Texture);
    CHECK(entry->path == "assets/coin.png");
    CHECK(entry->name == "coin");
    CHECK(std::filesystem::exists(temp.path() / "p" / "assets" / "coin.png"));
    // A second import of a file with the same name gets a new file name.
    const auto second = must(project->importFile(kExample / "assets" / "coin.png"));
    CHECK(project->asset(second)->path == "assets/coin-2.png");
    CHECK(project->asset(second)->name == "coin 2"); // names stay distinguishable

    // Broken and unsupported files are refused without changing the project.
    writeFile(temp.path() / "broken.png", "not an image");
    auto broken = project->importFile(temp.path() / "broken.png");
    REQUIRE_FALSE(broken.ok());
    CHECK(contains(broken.error().message, "broken.png"));
    writeFile(temp.path() / "notes.txt", "hello");
    CHECK_FALSE(project->importFile(temp.path() / "notes.txt").ok());
    CHECK(project->assets().size() == 3); // scene + 2 textures

    const auto script = must(project->importFile(kExample / "scripts" / "coin.luau"));
    CHECK(project->asset(script)->name == "coin"); // unique per type: an image and a script may share a name
    const ScriptInfo* info = project->scriptInfo(script);
    REQUIRE(info != nullptr);
    CHECK(info->compiled);
    REQUIRE(info->behaviour.properties.size() == 1);
    CHECK(info->behaviour.properties[0].first == "value");
    CHECK(info->behaviour.events == std::vector<std::string>{"TriggerEnter"});

    must(project->renameAsset(texture, "Gold Coin"));
    auto reopened = must(EditorProject::open(temp.path() / "p"));
    CHECK(reopened->asset(texture)->name == "Gold Coin");

    // An asset in use cannot be deleted; an unused one can.
    auto scene = must(project->loadScene(project->manifest().startScene));
    SceneDocument doc(project->manifest().startScene, scene);
    doc.createEntity(EntityPreset::Sprite, "Coin", kNullEntity, {}, texture);
    must(doc.save(*project));
    auto used = project->deleteAsset(texture);
    REQUIRE_FALSE(used.ok());
    CHECK(contains(used.error().message, "used by 1 entity"));
    CHECK(project->findUses(texture).size() == 1);
    must(project->deleteAsset(second));
    CHECK(project->asset(second) == nullptr);
    CHECK_FALSE(std::filesystem::exists(temp.path() / "p" / "assets" / "coin-2.png"));
    CHECK_FALSE(project->deleteAsset(project->manifest().startScene).ok());
}

TEST_CASE("Editor: script buffers, describe and errors") {
    TempFolder temp;
    auto project = must(EditorProject::create(temp.path() / "p", "P"));
    const auto id = must(project->createScript("Enemy Patrol"));
    CHECK(project->asset(id)->path == "scripts/enemy_patrol.luau");
    const std::string text = must(project->scriptText(id));
    CHECK(contains(text, "local EnemyPatrol = {"));
    const ScriptInfo* info = project->scriptInfo(id);
    REQUIRE(info != nullptr);
    CHECK(info->error.empty());
    CHECK(info->behaviour.properties.size() == 1); // speed
    CHECK(info->behaviour.events == std::vector<std::string>{"Start", "Tick"});

    // An unsaved broken edit is reported but not written.
    project->setScriptBuffer(id, "local x = \nreturn {}");
    CHECK(project->scriptBufferDirty(id));
    info = project->scriptInfo(id);
    REQUIRE(info != nullptr);
    CHECK_FALSE(info->compiled);
    CHECK(contains(info->error, "scripts/enemy_patrol.luau:2:"));
    CHECK_FALSE(project->buildScriptLibrary().ok());
    CHECK(contains(must(project->files().readText("scripts/enemy_patrol.luau")), "EnemyPatrol"));

    project->setScriptBuffer(id, "return { properties = { hp = 3, name = \"x\" } }");
    must(project->saveScript(id));
    CHECK_FALSE(project->anyScriptBufferDirty());
    info = project->scriptInfo(id);
    REQUIRE(info != nullptr);
    CHECK(info->behaviour.properties.size() == 2);
    CHECK(info->behaviour.properties[0].first == "hp");
}

TEST_CASE("Editor: saving refuses scenes that would not load") {
    TempFolder temp;
    auto project = must(EditorProject::create(temp.path() / "p", "P"));
    auto scene = must(project->loadScene(project->manifest().startScene));
    SceneDocument doc(project->manifest().startScene, scene);
    doc.createEntity(EntityPreset::Sprite, "No Texture");
    CHECK_FALSE(doc.problems(&project->assets()).empty());
    const Status saved = doc.save(*project);
    REQUIRE_FALSE(saved.ok());
    CHECK(contains(saved.error().message, "would not load"));
    CHECK(doc.isDirty());
}

TEST_CASE("Editor: the console log folds repeats and keeps a bounded history") {
    LogBuffer buffer(3);
    const auto previous = log::minLevel();
    log::setMinLevel(log::Level::Info);
    log::setConsoleOutput(false);
    log::info("one");
    log::error("boom");
    log::error("boom");
    log::warn("two");
    log::info("three");
    log::setConsoleOutput(true);
    log::setMinLevel(previous);
    const auto entries = buffer.entries();
    REQUIRE(entries.size() == 3);
    CHECK(entries[0].text == "boom");
    CHECK(entries[0].repeats == 2);
    CHECK(entries[2].text == "three");
    CHECK(buffer.count(log::Level::Error) == 2);
    buffer.clear();
    CHECK(buffer.entries().empty());
    CHECK(buffer.count(log::Level::Error) == 0);
}

// The milestone deliverable, driven through the same editor API that
// Studio's panels use: build a small game from an empty project without
// touching any project file directly, save it, then load and play it the way
// px-player does.
TEST_CASE("Editor: building, saving and playing a small game without editing files") {
    TempFolder temp;
    const auto folder = temp.path() / "coin-run";
    auto project = must(EditorProject::create(folder, "Coin Run"));

    // Assets: images and two behaviours.
    const auto tiles = must(project->importFile(kExample / "assets" / "tiles.png"));
    const auto hero = must(project->importFile(kExample / "assets" / "player.png"));
    const auto coinImage = must(project->importFile(kExample / "assets" / "coin.png"));
    const auto coinScript = must(project->importFile(kExample / "scripts" / "coin.luau"));
    const auto rulesScript = must(project->createScript("Rules"));
    project->setScriptBuffer(rulesScript, must(project->files().readText("scripts/rules.luau")));
    {
        // Write the rules in the script editor (an unsaved buffer at first).
        DirectoryFileSystem example(kExample);
        project->setScriptBuffer(rulesScript, must(example.readText("scripts/level_rules.luau")));
    }

    // The scene.
    auto start = must(project->loadScene(project->manifest().startScene));
    SceneDocument doc(project->manifest().startScene, std::move(start));
    const Entity ground = doc.createEntity(EntityPreset::Tilemap, "Ground", kNullEntity, {0, 0}, tiles);
    {
        doc.beginEdit("Tileset columns");
        doc.sceneForEdit().get<Tilemap>(ground).tilesetColumns = 4;
        doc.markChanged();
        doc.endEdit();
        doc.beginEdit("Paint");
        for (int x = 0; x < 20; ++x) {
            doc.setTile(ground, x, 8, 0);
            doc.setTile(ground, x, 9, 1);
        }
        doc.endEdit();
    }
    doc.createEntity(EntityPreset::Player, "Hero", kNullEntity, {24, 120}, hero);
    // The empty template's camera followed nothing; now it follows the hero.
    const Entity camera = doc.scene().findByName("Main Camera");
    CHECK(doc.scene().get<Camera>(camera).follow ==
          doc.scene().get<EntityInfo>(doc.scene().findByName("Hero")).id);
    const Entity coins = doc.createEntity(EntityPreset::Group, "Coins");
    const Entity coin = doc.createEntity(EntityPreset::Sprite, "Coin 1", coins, {80, 116}, coinImage);
    {
        doc.beginEdit("Coin setup");
        doc.sceneForEdit().get<Sprite>(coin).source = {0, 0, 16, 16};
        doc.markChanged();
        doc.endEdit();
    }
    must(doc.addComponent(coin, *findComponentType("BoxCollider")));
    {
        doc.beginEdit("Trigger");
        auto& collider = doc.sceneForEdit().get<BoxCollider>(coin);
        collider.isTrigger = true;
        collider.size = {10, 12};
        doc.markChanged();
        doc.endEdit();
    }
    must(doc.addComponent(coin, *findComponentType("Tags")));
    {
        doc.beginEdit("Tag");
        doc.sceneForEdit().get<Tags>(coin).values = {"coin"};
        doc.markChanged();
        doc.endEdit();
    }
    must(doc.attachScript(coin, coinScript));
    doc.setScriptProperty(coin, 0, "value", 5.0);
    // Two more coins by duplicating and moving.
    for (int i = 0; i < 2; ++i) {
        doc.duplicateSelection();
        doc.translateSelection({40, 0});
    }
    CHECK(names(doc.scene(), doc.scene().children(coins)) == std::vector<std::string>{"Coin 1", "Coin 2", "Coin 3"});
    const Entity rules = doc.createEntity(EntityPreset::Group, "Rules");
    must(doc.attachScript(rules, rulesScript));
    doc.setScriptProperty(rules, 0, "message", std::string("YOU WIN"));
    {
        doc.beginEdit("Scene settings");
        doc.sceneForEdit().settings().killPlaneY = 400.0f;
        doc.markChanged();
        doc.endEdit();
    }
    CHECK(doc.problems(&project->assets()).empty());

    // Play in the editor first: unsaved scene and unsaved script are used.
    {
        auto play = must(PlayMode::start(*project, doc.scene()));
        const runtime::ActionId right = play->session().actions().find("move_right");
        runtime::InputFrame input;
        input.held = 1u << right;
        for (int i = 0; i < 150; ++i) play->advance(input);
        CHECK(play->session().state().score == 15);
        CHECK(play->session().state().levelComplete);
        CHECK(play->session().state().completionMessage == "YOU WIN");
        // Pausing stops ticks until a single step is requested.
        play->setPaused(true);
        const auto ticks = play->session().tickCount();
        CHECK_FALSE(play->advance(input));
        play->requestStep();
        CHECK(play->advance(input));
        CHECK(play->session().tickCount() == ticks + 1);
    }
    CHECK(doc.scene().findByName("Coin 2") != kNullEntity); // playing never changes the edited scene

    // Save everything, then load it exactly as px-player does.
    must(doc.save(*project));
    must(project->saveAllScripts());
    CHECK_FALSE(doc.isDirty());
    DirectoryFileSystem files(folder);
    auto game = must(runtime::loadGame(files));
    CHECK(game.warnings.empty());
    auto session = must(runtime::startSession(game));
    runtime::InputFrame input;
    input.held = 1u << session->actions().find("move_right");
    for (int i = 0; i < 150; ++i) session->tick(input);
    CHECK(session->state().score == 15);
    CHECK(session->state().levelComplete);
    CHECK(session->state().scriptErrors == 0);
}

// --- Starter library ---------------------------------------------------------------------------

namespace {

NewProjectOptions ofType(project::GameType type) {
    NewProjectOptions options;
    options.gameType = type;
    return options;
}

int objectIndex(const Library& library, std::string_view name) {
    const auto& objects = library.manifest().objects;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (objects[i].name == name) return static_cast<int>(i);
    }
    throw std::runtime_error("no library object " + std::string(name));
}

std::size_t countAssets(const EditorProject& project, assets::AssetType type) {
    std::size_t n = 0;
    for (const auto& e : project.assets().entries()) n += e.type == type ? 1 : 0;
    return n;
}

} // namespace

TEST_CASE("Library: the starter library loads, and every item has what the Toolbox needs") {
    const Library library = must(Library::load(kLibrary));
    const auto& m = library.manifest();
    CHECK(m.assets.size() >= 20);
    CHECK(m.objects.size() >= 20);
    for (const auto& object : m.objects) {
        CAPTURE(object.name);
        CHECK_FALSE(object.category.empty());
        CHECK_FALSE(object.description.empty());
        CHECK_FALSE(object.iconTexture.isNil());
    }
    for (const auto& asset : m.assets) {
        CAPTURE(asset.entry.name);
        CHECK_FALSE(asset.category.empty());
        if (asset.entry.type == assets::AssetType::Texture) {
            const Vec2i size = library.imageSize(asset.entry.id);
            CHECK(size.x > 0);
            CHECK(size.y > 0);
        }
    }
    // Each game type has a player and a goal.
    for (const auto type : {project::GameType::Platformer, project::GameType::TopDown}) {
        bool player = false;
        bool goal = false;
        for (const auto& object : m.objects) {
            if (!object.fits(type)) continue;
            player |= object.category == "Characters";
            goal |= object.category == "Goals";
        }
        CHECK(player);
        CHECK(goal);
    }
}

TEST_CASE("Library: every object can be inserted into an empty project of its game type, saved and played") {
    const Library library = must(Library::load(kLibrary));
    for (const auto type : {project::GameType::Platformer, project::GameType::TopDown}) {
        CAPTURE(project::gameTypeName(type));
        TempFolder temp;
        auto project = must(EditorProject::create(temp.path() / "game", "Game", ofType(type)));
        CHECK(project->manifest().settings.gameType == type);
        SceneDocument doc(project->manifest().startScene, must(project->loadScene(project->manifest().startScene)));
        float x = 0.0f;
        for (std::size_t i = 0; i < library.manifest().objects.size(); ++i) {
            const auto& object = library.manifest().objects[i];
            if (!object.fits(type)) continue;
            CAPTURE(object.name);
            const auto inserted = must(insertLibraryObject(*project, doc, library, i, {x += 24.0f, 40.0f}));
            REQUIRE_FALSE(inserted.empty());
            // The object's entities are selected and in the scene; the first one is where it was put.
            CHECK(doc.selection().size() == inserted.size());
            if (const Entity first = inserted.front(); doc.scene().tryGet<Transform>(first) &&
                                                       !doc.scene().tryGet<Tilemap>(first)) {
                CHECK(doc.scene().worldPosition(first) == Vec2{x, 40.0f});
            }
        }
        CHECK(doc.problems(&project->assets()).empty());
        must(doc.save(*project));
        // Played as px-player would: no script errors, the player exists.
        auto reopened = must(EditorProject::open(temp.path() / "game"));
        DirectoryFileSystem files(temp.path() / "game");
        auto game = must(runtime::loadGame(files));
        auto session = must(runtime::startSession(game));
        for (int i = 0; i < 60; ++i) session->tick({});
        CHECK(session->state().scriptErrors == 0);
        CHECK_FALSE(session->player().isNull());
    }
}

TEST_CASE("Library: inserting twice reuses the copied files; template projects already have the shared art") {
    const Library library = must(Library::load(kLibrary));
    TempFolder temp;
    auto project = must(EditorProject::create(temp.path() / "p", "P"));
    SceneDocument doc(project->manifest().startScene, must(project->loadScene(project->manifest().startScene)));
    const int coin = objectIndex(library, "Coin");
    (void)must(insertLibraryObject(*project, doc, library, static_cast<std::size_t>(coin), {10, 10}));
    const std::size_t textures = countAssets(*project, assets::AssetType::Texture);
    const std::size_t scripts = countAssets(*project, assets::AssetType::Script);
    CHECK(textures == 1);
    CHECK(scripts == 1);
    (void)must(insertLibraryObject(*project, doc, library, static_cast<std::size_t>(coin), {40, 10}));
    CHECK(countAssets(*project, assets::AssetType::Texture) == textures);
    CHECK(countAssets(*project, assets::AssetType::Script) == scripts);
    CHECK(names(doc.scene(), doc.scene().entities()) == std::vector<std::string>{"Main Camera", "Coin", "Coin 2"});
    // One undo step per insert.
    doc.undo();
    CHECK(doc.scene().findByName("Coin 2").isNull());

    // A project made from the platformer example already has the coin.
    auto copy = must(EditorProject::create(temp.path() / "copy", "Copy", {.templateFolder = kExample}));
    const std::size_t before = copy->assets().size();
    SceneDocument level(copy->manifest().startScene, must(copy->loadScene(copy->manifest().startScene)));
    (void)must(insertLibraryObject(*copy, level, library, static_cast<std::size_t>(coin), {10, 10}));
    CHECK(copy->assets().size() == before);
}

TEST_CASE("Library: images become sprites or tilemaps; behaviours attach") {
    const Library library = must(Library::load(kLibrary));
    TempFolder temp;
    auto project = must(EditorProject::create(temp.path() / "p", "P", ofType(project::GameType::TopDown)));
    SceneDocument doc(project->manifest().startScene, must(project->loadScene(project->manifest().startScene)));
    assets::AssetId tiles;
    assets::AssetId tree;
    assets::AssetId hazard;
    for (const auto& a : library.manifest().assets) {
        if (a.entry.name == "Top-down Tiles") tiles = a.entry.id;
        if (a.entry.name == "Tree") tree = a.entry.id;
        if (a.entry.name == "Hazard") hazard = a.entry.id;
    }
    const Entity map = must(insertLibraryImage(*project, doc, library, tiles, {160, 90}));
    const auto& tilemap = doc.scene().get<Tilemap>(map);
    CHECK(tilemap.tilesetColumns == 8);
    CHECK(doc.scene().worldPosition(map) == Vec2{0, 16}); // 320 x 160 map centred on (160, 90), on tile lines
    const Entity treeEntity = must(insertLibraryImage(*project, doc, library, tree, {50, 50}));
    CHECK(doc.scene().get<Sprite>(treeEntity).size == Vec2{32, 32});
    must(attachLibraryScript(*project, doc, library, hazard, treeEntity));
    CHECK(doc.scene().get<Scripts>(treeEntity).items.size() == 1);
    CHECK(project->asset(hazard) != nullptr);
}

TEST_CASE("Library: broken library files are reported clearly") {
    TempFolder temp;
    std::filesystem::create_directories(temp.path() / "objects");
    const std::string head = R"({ "format": "px.library", "formatVersion": 1, "assets": [)";
    writeFile(temp.path() / "library.json", head + R"({"id": "aaaaaaaa-0000-4000-8000-000000000001",
        "type": "texture", "name": "Missing", "path": "art/missing.png"}]})");
    auto missing = Library::load(temp.path());
    REQUIRE_FALSE(missing.ok());
    CHECK(contains(missing.error().message, "missing.png is missing"));

    writeFile(temp.path() / "library.json", head + R"({"id": "aaaaaaaa-0000-4000-8000-000000000001",
        "type": "scene", "name": "S", "path": "s.scene.json"}]})");
    CHECK(contains(Library::load(temp.path()).error().message, "cannot be in a library"));

    writeFile(temp.path() / "library.json", head + R"(], "objects": [{"id": "aaaaaaaa-0000-4000-8000-000000000002",
        "name": "X", "gameTypes": ["racing"], "path": "objects/x.entities.json"}]})");
    CHECK(contains(Library::load(temp.path()).error().message, "unknown game type 'racing'"));

    writeFile(temp.path() / "library.json", head + R"(], "objects": [{"id": "aaaaaaaa-0000-4000-8000-000000000002",
        "name": "X", "path": "objects/x.entities.json"}]})");
    writeFile(temp.path() / "objects" / "x.entities.json", R"({"format": "px.entities", "formatVersion": 1,
        "sceneFormatVersion": 3, "entities": [{"id": "aaaaaaaa-0000-4000-8000-000000000003", "name": "X",
        "components": {"Sprite": {"texture": "aaaaaaaa-0000-4000-8000-000000000009", "size": [16, 16]}}}]})");
    auto foreign = Library::load(temp.path());
    REQUIRE_FALSE(foreign.ok());
    CHECK(contains(foreign.error().message, "Library object 'X'"));
    CHECK(contains(foreign.error().message, "not in the project"));
}
