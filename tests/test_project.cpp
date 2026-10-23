#include <doctest.h>

#include "px/core/FileSystem.hpp"
#include "px/project/Project.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/scene/ComponentInfo.hpp"

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

using namespace px;
using namespace px::project;
using namespace px::scene;

namespace {

const std::string kExampleDir = std::string(PX_EXAMPLES_DIR) + "/platformer";

constexpr const char* kTextureId = "aaaaaaaa-0000-4000-8000-000000000001";
constexpr const char* kSceneId = "aaaaaaaa-0000-4000-8000-000000000002";

std::string minimalProject(const std::string& extra = "") {
    return std::string(R"({
  "format": "px.project",
  "formatVersion": 1,
  "id": "aaaaaaaa-0000-4000-8000-0000000000ff",
  "name": "Test",
  "startScene": ")") + kSceneId + R"(",
  "input": { "actions": { "jump": ["Space"] } },
  "assets": [
    { "id": ")" + kTextureId + R"(", "type": "texture", "path": "t.png" },
    { "id": ")" + kSceneId + R"(", "type": "scene", "path": "s.scene.json" }
  ])" + extra + "\n}";
}

std::string sceneWithEntities(const std::string& entities) {
    return R"({ "format": "px.scene", "formatVersion": 1, "name": "T", "entities": [)" + entities + "] }";
}

// Returns the error message of parsing `json` as a scene (empty if it parsed).
std::string sceneError(const std::string& json, const assets::AssetRegistry* assets = nullptr) {
    auto result = parseScene(json, "test.scene.json", assets);
    return result.ok() ? std::string() : result.error().message;
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("Example project manifest loads") {
    DirectoryFileSystem fs(kExampleDir);
    auto manifest = loadProjectManifest(fs);
    REQUIRE_MESSAGE(manifest.ok(), (manifest.ok() ? "" : manifest.error().message));
    CHECK(manifest->name == "Example Platformer");
    CHECK(manifest->settings.tickRate == 60);
    CHECK(manifest->settings.display.virtualResolution == Vec2i{320, 180});
    CHECK(manifest->settings.display.scaling == ScalingMode::Integer);
    CHECK(manifest->input.find("jump") != nullptr);
    CHECK(manifest->input.find("jump")->keys.front() == Key::Space);
    CHECK(manifest->assets.size() == 9);
    CHECK(manifest->settings.scripting.apiVersion == 1);
}

TEST_CASE("Example scene loads with all expected content") {
    DirectoryFileSystem fs(kExampleDir);
    auto manifest = loadProjectManifest(fs);
    REQUIRE(manifest.ok());
    auto scene = loadScene(fs, manifest.value(), manifest->startScene);
    REQUIRE_MESSAGE(scene.ok(), (scene.ok() ? "" : scene.error().message));

    CHECK(scene->name() == "Level 1");
    CHECK(scene->settings().killPlaneY.has_value());
    const Entity player = scene->findByName("Player");
    REQUIRE_FALSE(player.isNull());
    CHECK(scene->tryGet<PlatformerController>(player) != nullptr);
    CHECK(scene->registry().count<Tags>() == 14);
    CHECK(scene->registry().count<Scripts>() == 16); // 14 coins, the moving platform, the level rules
    CHECK(scene->registry().count<Tilemap>() == 1);
    const Entity level = scene->findByName("Level");
    const auto* map = scene->tryGet<Tilemap>(level);
    REQUIRE(map != nullptr);
    CHECK(map->width == 84);
    CHECK(map->height == 14);
    CHECK(map->tileAt(0, 0) == 2); // stone wall
}

TEST_CASE("Scene serialization round-trips") {
    DirectoryFileSystem fs(kExampleDir);
    auto manifest = loadProjectManifest(fs);
    REQUIRE(manifest.ok());
    auto original = loadScene(fs, manifest.value(), manifest->startScene);
    REQUIRE(original.ok());

    const std::string first = serializeScene(original.value());
    auto reparsed = parseScene(first, "roundtrip", &manifest->assets);
    REQUIRE_MESSAGE(reparsed.ok(), (reparsed.ok() ? "" : reparsed.error().message));
    const std::string second = serializeScene(reparsed.value());
    CHECK(first == second); // serialization is deterministic and lossless

    CHECK(reparsed->entityCount() == original->entityCount());
    const Entity p1 = original->findByName("Player");
    const Entity p2 = reparsed->findByName("Player");
    CHECK(original->tryGet<EntityInfo>(p1)->id == reparsed->tryGet<EntityInfo>(p2)->id);
    CHECK(original->tryGet<Transform>(p1)->position == reparsed->tryGet<Transform>(p2)->position);
    CHECK(original->tryGet<PlatformerController>(p1)->jumpCutMultiplier ==
          reparsed->tryGet<PlatformerController>(p2)->jumpCutMultiplier);
    // Floats are written in their shortest form.
    CHECK(contains(first, "\"jumpCutMultiplier\": 0.45"));
    CHECK(contains(first, "\"position\": ["));
}

TEST_CASE("Project manifest round-trips") {
    DirectoryFileSystem fs(kExampleDir);
    auto manifest = loadProjectManifest(fs);
    REQUIRE(manifest.ok());
    const std::string text = serializeProjectManifest(manifest.value());
    auto again = parseProjectManifest(text, "roundtrip");
    REQUIRE_MESSAGE(again.ok(), (again.ok() ? "" : again.error().message));
    CHECK(again->id == manifest->id);
    CHECK(again->startScene == manifest->startScene);
    CHECK(again->input.actions.size() == manifest->input.actions.size());
    CHECK(serializeProjectManifest(again.value()) == text);
}

TEST_CASE("Manifest validation errors are specific") {
    CHECK(parseProjectManifest(minimalProject(), "p.json").ok());

    auto syntax = parseProjectManifest("{ \"format\": ", "p.json");
    REQUIRE_FALSE(syntax.ok());
    CHECK(contains(syntax.error().message, "p.json: invalid JSON"));

    auto wrongKind = parseProjectManifest(R"({"format": "px.scene", "formatVersion": 1})", "p.json");
    CHECK(contains(wrongKind.error().message, "expected \"format\": \"px.project\""));

    auto future = parseProjectManifest(R"({"format": "px.project", "formatVersion": 99})", "p.json");
    CHECK(contains(future.error().message, "format version 99"));
    CHECK(contains(future.error().message, "update the engine"));

    auto badKey = parseProjectManifest(minimalProject(R"(, "input2": {})"), "p.json");
    CHECK(contains(badKey.error().message, "unknown field 'input2' (did you mean 'input'?)"));

    std::string badAction = minimalProject();
    badAction.replace(badAction.find("\"Space\""), 7, "\"Spcae\"");
    CHECK(contains(parseProjectManifest(badAction, "p.json").error().message, "unknown key 'Spcae'"));

    std::string missingStart = minimalProject();
    missingStart.replace(missingStart.find(kSceneId), 36, "bbbbbbbb-0000-4000-8000-000000000000");
    CHECK(contains(parseProjectManifest(missingStart, "p.json").error().message, "not listed in \"assets\""));

    std::string escape = minimalProject();
    escape.replace(escape.find("t.png"), 5, "../t.png");
    CHECK(contains(parseProjectManifest(escape, "p.json").error().message, "may not contain '..'"));
}

TEST_CASE("Scene validation errors point at the problem") {
    const std::string goodEntity = R"({"id": "cccccccc-0000-4000-8000-000000000001", "name": "Box",
        "components": {"Transform": {"position": [1, 2]}}})";
    CHECK(sceneError(sceneWithEntities(goodEntity)).empty());

    // Misspelled component, with a suggestion.
    const std::string typo = R"({"id": "cccccccc-0000-4000-8000-000000000001", "name": "Box",
        "components": {"Transfrom": {}}})";
    const std::string typoError = sceneError(sceneWithEntities(typo));
    CHECK(contains(typoError, "entities[0] ('Box').components: unknown component 'Transfrom' (did you mean 'Transform'?)"));

    // Misspelled field.
    const std::string field = R"({"id": "cccccccc-0000-4000-8000-000000000001", "name": "P",
        "components": {"PlatformerController": {"jumpspeed": 3}}})";
    CHECK(contains(sceneError(sceneWithEntities(field)), "unknown field 'jumpspeed' (did you mean 'jumpSpeed'?)"));

    // Wrong value type.
    const std::string type = R"({"id": "cccccccc-0000-4000-8000-000000000001",
        "components": {"Transform": {"position": [1]}}})";
    CHECK(contains(sceneError(sceneWithEntities(type)), "entities[0].components.Transform.position: expected [x, y]"));

    // Duplicate ids.
    const std::string dup = goodEntity + "," + goodEntity;
    CHECK(contains(sceneError(sceneWithEntities(dup)), "is used by both"));

    // Missing texture asset.
    assets::AssetRegistry registry;
    REQUIRE(registry.add({assets::AssetId{*Guid::parse(kTextureId)}, assets::AssetType::Texture, "t", "t.png",
                          assets::TextureFilter::Nearest}).ok());
    const std::string sprite = R"({"id": "cccccccc-0000-4000-8000-000000000001",
        "components": {"Sprite": {"texture": "dddddddd-0000-4000-8000-000000000000"}}})";
    CHECK(contains(sceneError(sceneWithEntities(sprite), &registry), "Sprite.texture: refers to asset"));

    // Camera following a missing entity.
    const std::string camera = R"({"id": "cccccccc-0000-4000-8000-000000000001",
        "components": {"Camera": {"follow": "eeeeeeee-0000-4000-8000-000000000000"}}})";
    CHECK(contains(sceneError(sceneWithEntities(camera)), "no entity with id"));

    // Controller without its required components.
    const std::string controller = R"({"id": "cccccccc-0000-4000-8000-000000000001",
        "components": {"PlatformerController": {}}})";
    CHECK(contains(sceneError(sceneWithEntities(controller)), "requires Transform, BoxCollider and KinematicBody"));
}

TEST_CASE("Tilemap rows are validated") {
    const auto tilemapScene = [](const std::string& rows) {
        return sceneWithEntities(std::string(R"({"id": "cccccccc-0000-4000-8000-000000000001",
            "components": {"Tilemap": {"tileset": ")") + kTextureId + R"(", "rows": )" + rows + "}}}");
    };
    auto ok = parseScene(tilemapScene(R"(["0 1 .", ". . 3"])"), "t");
    REQUIRE(ok.ok());
    const auto* map = ok->tryGet<Tilemap>(ok->entities().front());
    CHECK(map->width == 3);
    CHECK(map->height == 2);
    CHECK(map->tiles == std::vector<int>{0, 1, -1, -1, -1, 3});

    CHECK(contains(sceneError(tilemapScene(R"(["0 1 .", "0 1"])")), "rows[1]: has 2 tiles, but row 0 has 3"));
    CHECK(contains(sceneError(tilemapScene(R"(["0 x"])")), "'x' is not a tile index"));
    CHECK(contains(sceneError(tilemapScene(R"(["0 -1"])")), "'-1' is not a tile index"));
}

TEST_CASE("Scene hierarchy is saved and loaded, in any file order") {
    // Child listed before its parent; loading restores hierarchy order.
    const std::string json = sceneWithEntities(R"(
        {"id": "cccccccc-0000-4000-8000-000000000002", "name": "Child", "parent": "cccccccc-0000-4000-8000-000000000001",
         "components": {"Transform": {"position": [5, 0]}}},
        {"id": "cccccccc-0000-4000-8000-000000000001", "name": "Parent",
         "components": {"Transform": {"position": [100, 10]}}})");
    auto scene = parseScene(json, "h.scene.json");
    REQUIRE_MESSAGE(scene.ok(), (scene.ok() ? "" : scene.error().message));
    const Entity parent = scene->findByName("Parent");
    const Entity child = scene->findByName("Child");
    CHECK(scene->parent(child) == parent);
    CHECK(scene->entities().front() == parent);
    CHECK(scene->worldPosition(child) == Vec2{105, 10});

    const std::string saved = serializeScene(scene.value());
    CHECK(contains(saved, "\"parent\": \"cccccccc-0000-4000-8000-000000000001\""));
    auto again = parseScene(saved, "again");
    REQUIRE(again.ok());
    CHECK(serializeScene(again.value()) == saved);

    // A parent that does not exist, and a loop.
    const std::string missing = sceneWithEntities(R"(
        {"id": "cccccccc-0000-4000-8000-000000000002", "name": "Child", "parent": "cccccccc-0000-4000-8000-000000000009"})");
    CHECK(contains(sceneError(missing), "entities[0] ('Child').parent: no entity with id"));
    const std::string loop = sceneWithEntities(R"(
        {"id": "cccccccc-0000-4000-8000-000000000001", "name": "A", "parent": "cccccccc-0000-4000-8000-000000000002"},
        {"id": "cccccccc-0000-4000-8000-000000000002", "name": "B", "parent": "cccccccc-0000-4000-8000-000000000001"})");
    CHECK(contains(sceneError(loop), "form a loop"));
}

TEST_CASE("Scene format 2 Script components become Scripts lists") {
    const std::string v2 = std::string(R"({ "format": "px.scene", "formatVersion": 2, "name": "T", "entities": [
        {"id": "cccccccc-0000-4000-8000-000000000001", "name": "S",
         "components": {"Script": {"script": "dddddddd-0000-4000-8000-000000000001", "properties": {"speed": 2}}}}]})");
    auto scene = parseScene(v2, "v2.scene.json");
    REQUIRE_MESSAGE(scene.ok(), (scene.ok() ? "" : scene.error().message));
    const auto* scripts = scene->tryGet<Scripts>(scene->findByName("S"));
    REQUIRE(scripts != nullptr);
    REQUIRE(scripts->items.size() == 1);
    CHECK(scripts->items[0].script.toString() == "dddddddd-0000-4000-8000-000000000001");
    REQUIRE(scripts->items[0].findProperty("speed") != nullptr);
    CHECK(std::get<double>(scripts->items[0].findProperty("speed")->value) == 2.0);
    const std::string saved = serializeScene(scene.value());
    CHECK(contains(saved, "\"Scripts\": ["));
    CHECK_FALSE(contains(saved, "\"Script\":"));
}

TEST_CASE("Value ranges come from the component descriptors") {
    const auto withComponent = [](const std::string& components) {
        return sceneWithEntities(R"({"id": "cccccccc-0000-4000-8000-000000000001", "name": "E", "components": )" +
                                 components + "}");
    };
    CHECK(contains(sceneError(withComponent(R"({"Transform": {}, "BoxCollider": {"size": [0, 4]}, "KinematicBody": {},
        "PlatformerController": {}})")),
                   "entities[0] ('E').components.BoxCollider.size: must be positive"));
    CHECK(contains(sceneError(withComponent(R"({"Transform": {}, "BoxCollider": {}, "KinematicBody": {},
        "PlatformerController": {"jumpCutMultiplier": 2}})")),
                   "PlatformerController.jumpCutMultiplier: must be between 0 and 1"));
    CHECK(contains(sceneError(withComponent(R"({"Transform": {}, "BoxCollider": {}, "KinematicBody": {},
        "PlatformerController": {"gravity": -1}})")),
                   "PlatformerController.gravity: may not be negative"));
    CHECK(contains(sceneError(withComponent(R"({"Camera": {"zoom": 100}})")),
                   "Camera.zoom: must be greater than 0 and at most 64"));
    CHECK(contains(sceneError(withComponent(R"({"SpriteAnimation": {"frames": [[0, 0, 8, 8]], "framesPerSecond": 0}})")),
                   "SpriteAnimation.framesPerSecond: must be greater than 0 and at most 240"));
    CHECK(contains(sceneError(withComponent(R"({"Tags": ["ok", ""]})")), "entries must be 1 to 64 characters"));
    // Runtime fields are not part of files.
    CHECK(contains(sceneError(withComponent(R"({"KinematicBody": {"velocity": [1, 2]}})")), "unknown field 'velocity'"));
}

TEST_CASE("Every component survives a save and load with its descriptor") {
    // Build an entity with every component type, using non-default values
    // where the descriptors allow, and check the file round-trips exactly.
    assets::AssetRegistry registry;
    const assets::AssetId texture{*Guid::parse(kTextureId)};
    const assets::AssetId script{*Guid::parse("aaaaaaaa-0000-4000-8000-000000000003")};
    REQUIRE(registry.add({texture, assets::AssetType::Texture, "t", "t.png", assets::TextureFilter::Nearest}).ok());
    REQUIRE(registry.add({script, assets::AssetType::Script, "s", "s.luau", assets::TextureFilter::Nearest}).ok());

    Scene scene;
    const Entity e = scene.createEntity("Everything");
    for (const ComponentInfo& c : componentTypes()) c.add(scene, e);
    scene.tryGet<Sprite>(e)->texture = texture;
    scene.tryGet<SpriteAnimation>(e)->frames = {Rect{0, 0, 8, 8}, Rect{8, 0, 8, 8}};
    auto& map = *scene.tryGet<Tilemap>(e);
    map.tileset = texture;
    map.width = 2;
    map.height = 1;
    map.tiles = {3, -1};
    scene.tryGet<Tags>(e)->values = {"a", "b"};
    scene.tryGet<Scripts>(e)->items = {AttachedScript{script, {{"n", 1.5}, {"v", Vec2{1, 2}}}}, AttachedScript{script, {}}};
    scene.tryGet<Camera>(e)->follow = scene.tryGet<EntityInfo>(e)->id;
    CHECK(validateEntity(scene, e, &registry).empty());

    const std::string first = serializeScene(scene);
    auto loaded = parseScene(first, "all", &registry);
    REQUIRE_MESSAGE(loaded.ok(), (loaded.ok() ? "" : loaded.error().message));
    CHECK(serializeScene(loaded.value()) == first);
    for (const ComponentInfo& c : componentTypes()) {
        CHECK_MESSAGE(c.has(loaded.value(), loaded->entities().front()), c.name);
    }
    CHECK(loaded->tryGet<Scripts>(loaded->entities().front())->items.size() == 2);

    // Validation catches what the loader would reject.
    scene.tryGet<Sprite>(e)->texture = assets::AssetId{*Guid::parse(kSceneId)};
    REQUIRE(registry.add({assets::AssetId{*Guid::parse(kSceneId)}, assets::AssetType::Scene, "sc", "s.scene.json",
                          assets::TextureFilter::Nearest}).ok());
    const auto problems = validateEntity(scene, e, &registry);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].component == "Sprite");
    CHECK(problems[0].field == "texture");
    CHECK(contains(problems[0].message, "is a scene, not a texture"));
    scene.registry().remove<KinematicBody>(e);
    CHECK(contains(validateEntity(scene, e, &registry).back().message,
                   "requires Transform, BoxCollider and KinematicBody"));
}

TEST_CASE("Descriptors: names, labels and requirements") {
    std::vector<std::string> seen;
    for (const ComponentInfo& c : componentTypes()) {
        CHECK(std::find(seen.begin(), seen.end(), std::string(c.name)) == seen.end());
        seen.emplace_back(c.name);
        CHECK_FALSE(c.description.empty());
        for (const FieldInfo& f : c.fields) {
            CHECK_MESSAGE(!f.description.empty(), c.name, ".", f.name);
            CHECK(f.access != nullptr);
        }
    }
    CHECK(findComponentType("PlatformerController")->findField("jumpCutMultiplier")->displayLabel() ==
          "Jump Cut Multiplier");
    CHECK(findComponentType("Sprite")->findField("flipX")->displayLabel() == "Flip X");
    CHECK(findComponentType("BoxCollider")->findField("isTrigger")->displayLabel() == "Trigger");

    Scene scene;
    const Entity e = scene.createEntity("E");
    const ComponentInfo& controller = *findComponentType("PlatformerController");
    const auto missing = missingRequirements(scene, e, controller);
    REQUIRE(missing.size() == 3);
    CHECK(missing[0]->name == "Transform");
    CHECK(missing[1]->name == "BoxCollider");
    CHECK(missing[2]->name == "KinematicBody");
    for (const auto* m : missing) m->add(scene, e);
    controller.add(scene, e);
    REQUIRE(dependents(scene, e, *findComponentType("BoxCollider")).size() == 1);
    CHECK(dependents(scene, e, *findComponentType("Sprite")).empty());
}

TEST_CASE("Copy and paste of entities keeps internal references") {
    Scene scene;
    const Entity group = scene.createEntity("Group");
    const Entity hero = scene.createEntity("Hero", group);
    scene.set(hero, Transform{{3, 4}});
    const Entity cam = scene.createEntity("Cam", group);
    scene.set(cam, Camera{scene.tryGet<EntityInfo>(hero)->id});
    const Entity outside = scene.createEntity("Outside");
    const Entity follower = scene.createEntity("Follower");
    scene.set(follower, Camera{scene.tryGet<EntityInfo>(outside)->id});

    const Entity roots[] = {group, follower, hero}; // hero is inside group: copied once
    const std::string clip = serializeEntities(scene, roots);
    CHECK(contains(clip, "\"format\": \"px.entities\""));

    auto pasted = pasteEntities(scene, clip, nullptr);
    REQUIRE_MESSAGE(pasted.ok(), (pasted.ok() ? "" : pasted.error().message));
    REQUIRE(pasted->size() == 2);
    const Entity groupCopy = (*pasted)[0];
    CHECK(scene.tryGet<EntityInfo>(groupCopy)->name == "Group");
    const auto kids = scene.children(groupCopy);
    REQUIRE(kids.size() == 2);
    // The copied camera follows the copied hero, not the original.
    CHECK(scene.tryGet<Camera>(kids[1])->follow == scene.tryGet<EntityInfo>(kids[0])->id);
    // A reference to an entity outside the copied set is kept (it exists in this scene).
    CHECK(scene.tryGet<Camera>((*pasted)[1])->follow == scene.tryGet<EntityInfo>(outside)->id);

    // Pasting into another scene drops references that do not exist there.
    Scene other;
    auto intoOther = pasteEntities(other, clip, nullptr);
    REQUIRE(intoOther.ok());
    CHECK(other.tryGet<Camera>((*intoOther)[1])->follow.isNil());

    CHECK_FALSE(pasteEntities(scene, "{\"format\": \"px.scene\"}", nullptr).ok());
}

TEST_CASE("Component reference document is up to date") {
    // Regenerate with: px-headless --print-component-reference > docs/components.md
    DirectoryFileSystem docs(PX_DOCS_DIR);
    auto text = docs.readText("components.md");
    REQUIRE_MESSAGE(text.ok(), "docs/components.md is missing");
    std::string onDisk = text.value();
    std::erase(onDisk, '\r'); // git may check text files out with CRLF line endings (Windows)
    const bool upToDate = onDisk == generateComponentReferenceMarkdown();
    CHECK_MESSAGE(upToDate, "docs/components.md is out of date; regenerate it with px-headless --print-component-reference");
}

TEST_CASE("The example project is stored exactly as Studio saves it") {
    // So opening and saving it in Studio produces no changes. Regenerate with
    // tools/generate_example_project.py (which runs px-headless --resave).
    DirectoryFileSystem fs(kExampleDir);
    auto manifestText = fs.readText("project.json");
    REQUIRE(manifestText.ok());
    std::string manifestOnDisk = manifestText.value();
    std::erase(manifestOnDisk, '\r');
    auto manifest = parseProjectManifest(manifestOnDisk, "project.json");
    REQUIRE(manifest.ok());
    CHECK(serializeProjectManifest(manifest.value()) == manifestOnDisk);
    for (const auto& entry : manifest->assets.entries()) {
        if (entry.type != assets::AssetType::Scene) continue;
        auto text = fs.readText(entry.path);
        REQUIRE(text.ok());
        std::string onDisk = text.value();
        std::erase(onDisk, '\r');
        auto scene = parseScene(onDisk, entry.path, &manifest->assets);
        REQUIRE(scene.ok());
        CHECK_MESSAGE(serializeScene(scene.value()) == onDisk, entry.path, " is not in canonical form; run "
                                                                           "px-headless examples/platformer --resave");
    }
}
