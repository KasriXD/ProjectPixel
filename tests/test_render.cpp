#include <doctest.h>

#include "px/core/FileSystem.hpp"
#include "px/project/Project.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/render/Camera2D.hpp"
#include "px/render/SceneRenderer.hpp"
#include "px/render/TextureCache.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/runtime/GameSession.hpp"

#include <string>
#include <vector>

using namespace px;
using namespace px::render;
using namespace px::scene;

namespace {

// Records draw calls instead of drawing.
class RecordingRenderer final : public IRenderer2D {
public:
    struct Texture {
        int width;
        int height;
        assets::TextureFilter filter;
    };
    std::vector<Texture> textures;
    std::vector<SpriteDraw> sprites;
    int destroyed = 0;

    TextureHandle createTexture(const assets::Image& image, assets::TextureFilter filter) override {
        textures.push_back({image.width, image.height, filter});
        return static_cast<TextureHandle>(textures.size());
    }
    void destroyTexture(TextureHandle) override { ++destroyed; }
    Vec2i textureSize(TextureHandle t) const override {
        return {textures[t - 1].width, textures[t - 1].height};
    }
    void beginFrame(Color) override {}
    void drawSprite(const SpriteDraw& draw) override { sprites.push_back(draw); }
    void fillRect(const Rect&, Color) override {}
    void drawRect(const Rect&, Color) override {}
    void drawDebugText(Vec2, std::string_view, Color) override {}
    void endFrame() override {}
};

const assets::AssetId kTex{*Guid::parse("aaaaaaaa-0000-4000-8000-000000000001")};
const assets::AssetId kMissing{*Guid::parse("aaaaaaaa-0000-4000-8000-000000000009")};

} // namespace

TEST_CASE("Camera2D maps between world and screen") {
    Camera2D cam;
    cam.center = {100, 50};
    cam.viewport = {320, 180};
    cam.zoom = 1.0f;
    CHECK(cam.viewOrigin() == Vec2{-60, -40});
    CHECK(cam.worldToScreen({100, 50}) == Vec2{160, 90});
    CHECK(cam.screenToWorld({0, 0}) == Vec2{-60, -40});
    CHECK(cam.visibleWorldRect() == Rect{-60, -40, 320, 180});

    cam.zoom = 2.0f;
    CHECK(cam.worldToScreen({100, 50}) == Vec2{160, 90});
    CHECK(cam.visibleWorldRect() == Rect{20, 5, 160, 90});
    const Vec2 world = cam.screenToWorld({37, 81});
    const Vec2 back = cam.worldToScreen(world);
    CHECK(back.x == doctest::Approx(37.0f));
    CHECK(back.y == doctest::Approx(81.0f));

    // Pixel snapping keeps the view origin on whole screen pixels.
    cam.zoom = 1.0f;
    cam.center = {100.4f, 50.6f};
    CHECK(cam.viewOrigin() == Vec2{-60, -39});
    cam.pixelSnap = false;
    CHECK(cam.viewOrigin().x == doctest::Approx(-59.6f));
}

TEST_CASE("Scene renderer orders by layer, culls, and snaps to pixels") {
    MemoryFileSystem files;
    assets::AssetRegistry registry;
    REQUIRE(registry.add({kTex, assets::AssetType::Texture, "tex", "tex.png", assets::TextureFilter::Nearest}).ok());
    // 1x1 PNG.
    const std::vector<std::uint8_t> png = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
        0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF,
        0x56, 0xC7, 0x2F, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    files.addFile("tex.png", png);

    Scene scene;
    const auto addSprite = [&](const char* name, Vec2 pos, int layer) {
        const Entity e = scene.createEntity(name);
        scene.set(e, Transform{pos});
        Sprite s;
        s.texture = kTex;
        s.layer = layer;
        s.size = {16, 16};
        s.tint = Color{static_cast<std::uint8_t>(layer + 100), 0, 0, 255};
        scene.set(e, s);
        return e;
    };
    addSprite("front", {10.3f, 10}, 5);
    addSprite("back", {20, 20}, -5);
    addSprite("offscreen", {5000, 5000}, 0);
    const Entity hidden = addSprite("hidden", {30, 30}, 0);
    scene.get<Sprite>(hidden).visible = false;

    const Entity map = scene.createEntity("map");
    scene.set(map, Transform{});
    Tilemap tm;
    tm.tileset = kTex;
    tm.width = 3;
    tm.height = 1;
    tm.tiles = {0, -1, 1};
    tm.tilesetColumns = 2;
    tm.layer = 0;
    scene.set(map, tm);

    RecordingRenderer renderer;
    TextureCache cache(renderer, files, registry);
    Camera2D cam;
    cam.center = {160, 90};
    const SceneRenderStats stats = renderScene(scene, cam, renderer, cache);

    CHECK(stats.sprites == 2);
    CHECK(stats.culledSprites == 1);
    CHECK(stats.tiles == 2);
    REQUIRE(renderer.sprites.size() == 4);
    // back (-5), tile, tile (layer 0), front (5)
    CHECK(renderer.sprites[0].tint.r == 95);
    CHECK(renderer.sprites[1].source == Rect{0, 0, 16, 16});
    CHECK(renderer.sprites[2].source == Rect{16, 0, 16, 16});
    CHECK(renderer.sprites[2].dest == Rect{32, 0, 16, 16});
    CHECK(renderer.sprites[3].tint.r == 105);
    // Sprite centred at x=10.3 with a 16px width: left edge 2.3 snaps to 2.
    CHECK(renderer.sprites[3].dest.x == doctest::Approx(2.0f));
    // The texture was loaded once and shared.
    CHECK(renderer.textures.size() == 1);
}

TEST_CASE("Missing textures render as a placeholder instead of failing") {
    MemoryFileSystem files;
    assets::AssetRegistry registry;
    REQUIRE(registry.add({kMissing, assets::AssetType::Texture, "gone", "gone.png", assets::TextureFilter::Linear}).ok());
    RecordingRenderer renderer;
    {
        TextureCache cache(renderer, files, registry);
        const TextureHandle a = cache.get(kMissing);
        const TextureHandle b = cache.get(kMissing);
        CHECK(a == b);
        CHECK(cache.isMissing(a));
        CHECK(renderer.textures.size() == 1); // only the placeholder
        CHECK(renderer.textures[0].width == 8);
    }
    CHECK(renderer.destroyed == 1); // cache releases its textures
}

TEST_CASE("Example level renders the visible part of the level") {
    DirectoryFileSystem fs(std::string(PX_EXAMPLES_DIR) + "/platformer");
    auto manifest = project::loadProjectManifest(fs);
    REQUIRE(manifest.ok());
    auto scene = project::loadScene(fs, manifest.value(), manifest->startScene);
    REQUIRE(scene.ok());
    // As in the game: the session positions the camera on the player.
    auto created = runtime::GameSession::create(manifest.value(), std::move(scene.value()), nullptr);
    // The example scene has Script components; without a script library the session must refuse to start.
    CHECK_FALSE(created.ok());
    auto game = runtime::loadGame(fs);
    REQUIRE(game.ok());
    auto started = runtime::startSession(game.value());
    REQUIRE(started.ok());
    runtime::GameSession& session = *started.value();
    RecordingRenderer renderer;
    TextureCache cache(renderer, fs, manifest->assets);
    const Camera2D cam = cameraForScene(session.scene(), {320, 180});
    CHECK(cam.center == Vec2{160, 134}); // clamped to the level's left and bottom edges
    const SceneRenderStats stats = renderScene(session.scene(), cam, renderer, cache);
    CHECK(stats.tiles > 50);
    CHECK(stats.sprites > 3);
    CHECK(stats.culledSprites > 3);
    CHECK(renderer.textures.size() == 4); // tiles, player, coin, scenery
    for (const auto& t : renderer.textures) CHECK(t.filter == assets::TextureFilter::Nearest);
}
