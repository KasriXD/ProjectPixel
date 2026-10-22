#include <doctest.h>

#include "px/core/FileSystem.hpp"
#include "px/project/Project.hpp"
#include "px/project/SceneFormat.hpp"

#include <string>

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
    CHECK(scene->registry().count<Script>() == 16); // 14 coins, the moving platform, the level rules
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
