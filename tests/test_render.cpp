#include <doctest.h>

#include "px/core/FileSystem.hpp"
#include "px/core/Utf8.hpp"
#include "px/fontbake/FontBaker.hpp"
#include "px/render/Text.hpp"
#include "px/project/Project.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/render/Camera2D.hpp"
#include "px/render/SceneRenderer.hpp"
#include "px/render/TextureCache.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/runtime/GameSession.hpp"

#include <fstream>
#include <sstream>
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
    std::vector<std::pair<Rect, Color>> fills;
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
    void fillRect(const Rect& rect, Color color) override { fills.push_back({rect, color}); }
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

// --- text -----------------------------------------------------------------------------------

namespace {

// A font where every letter is 4 wide (advance 5) and 6 tall; line height 8.
project::FontData testFont() {
    project::FontData f;
    f.texture = kTex;
    f.size = 6;
    f.lineHeight = 8;
    f.ascent = 6;
    f.glyphs.push_back({U' ', {}, {}, 5});
    for (char32_t c : std::u32string(U"?ABCabcéЖ")) f.glyphs.push_back({c, {0, 0, 4, 6}, {0, 0}, 5});
    std::sort(f.glyphs.begin(), f.glyphs.end(), [](const auto& a, const auto& b) { return a.codepoint < b.codepoint; });
    return f;
}

} // namespace

TEST_CASE("UTF-8: decoding never gets stuck; broken bytes become U+FFFD") {
    const std::string text = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80" "b";
    std::size_t pos = 0;
    std::u32string out;
    while (pos < text.size()) out.push_back(utf8::next(text, pos));
    CHECK(out == U"aé€\U0001F600b");
    for (const std::string bad : {"\x80", "\xc3", "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xff"}) {
        std::size_t p = 0;
        CHECK(utf8::next(bad, p) == utf8::kReplacement);
        CHECK(p == 1);
    }
    std::string round;
    for (char32_t c : out) utf8::append(round, c);
    CHECK(round == text);
    CHECK(utf8::truncate("a\xc3\xa9", 2) == "a");
    CHECK(utf8::truncate("abc", 5) == "abc");
}

TEST_CASE("Text layout: lines, wrapping, alignment, padding and missing letters") {
    const project::FontData font = testFont();
    auto one = layoutText(font, "AB C");
    CHECK(one.lines == 1);
    CHECK(one.size == Vec2{20, 8}); // 4 advances; the line ends on a letter
    CHECK(one.glyphs.size() == 3);
    CHECK(one.glyphs[2].dest.x == 15.0f);

    // New lines, trailing spaces not counted, unknown letters as "?", broken UTF-8 too.
    auto lines = layoutText(font, "AB  \nC\xe2\x82\xac\xff");
    CHECK(lines.lines == 2);
    CHECK(lines.size == Vec2{15, 16});
    CHECK(lines.glyphs.size() == 5);
    CHECK(lines.glyphs[2].dest.y == 8.0f);

    // Wrapping at spaces; a long word breaks where it must.
    TextLayoutOptions wrap;
    wrap.wrapWidth = 22;
    auto wrapped = layoutText(font, "AB CA BC AAAAAAAAA", wrap);
    CHECK(wrapped.lines == 6); // "AB" / "CA" / "BC" / "AAAA" / "AAAA" / "A" (three letters need 24)
    CHECK(wrapped.size.x <= 22.0f);
    // Alignment and padding.
    TextLayoutOptions centred;
    centred.align = 0.5f;
    centred.padding = 2;
    centred.lineSpacing = 1;
    auto c = layoutText(font, "ABC\nA", centred);
    CHECK(c.size == Vec2{19, 21}); // 15 + 4 wide; 8 + 1 + 8 + 4 tall
    CHECK(c.glyphs[3].dest == Rect{2 + 5, 2 + 9, 4, 6});
    CHECK(layoutText(font, "").glyphs.empty());

    CHECK(anchorFactor("top-left") == Vec2{0, 0});
    CHECK(anchorFactor("bottom-right") == Vec2{1, 1});
    CHECK(anchorFactor("top") == Vec2{0.5f, 0});
    CHECK(anchorFactor("left") == Vec2{0, 0.5f});
    CHECK(anchorFactor("center") == Vec2{0.5f, 0.5f});
}

TEST_CASE("Text rendering: world texts by layer, screen texts on top, each HUD only to its player") {
    RecordingRenderer renderer;
    MemoryFileSystem files;
    assets::AssetRegistry registry;
    TextureCache textures(renderer, files, registry);
    FontCache fonts(renderer, textures, files, registry);
    const LoadedFont builtIn = fonts.builtIn();
    REQUIRE(builtIn.data != nullptr);
    CHECK(builtIn.data->size == 8);
    CHECK(builtIn.data->lineHeight == 10);
    CHECK(builtIn.data->texture.guid == kBuiltInFontTexture);
    CHECK(builtIn.data->find(U'A') != nullptr);
    CHECK(builtIn.data->find(U'Ж') != nullptr); // Cyrillic
    CHECK(builtIn.data->glyphs.size() > 500);
    CHECK(fonts.get(kMissing).data == builtIn.data); // not a font: the built-in one, reported once

    Scene scene;
    const Entity sign = scene.createEntity("Sign");
    scene.set(sign, Transform{{100, 50}});
    Text world;
    world.text = "HI";
    world.anchor = "bottom";
    world.layer = 5;
    world.background = Color{0, 0, 0, 128};
    world.padding = 1;
    scene.set(sign, world);
    const Entity behind = scene.createEntity("Behind");
    scene.set(behind, Transform{{100, 50}});
    Sprite sprite;
    sprite.texture = kMissing;
    sprite.size = {4, 4};
    sprite.layer = 10;
    scene.set(behind, sprite);

    const Entity mine = scene.createEntity("Mine");
    scene.set(mine, Transform{{0, 0}});
    scene.set(mine, Player{{}, 1, "Ada"});
    const Entity hud = scene.createEntity("Score", mine);
    scene.set(hud, Transform{{-4, 4}});
    Text score;
    score.text = "9";
    score.screen = true;
    score.anchor = "top-right";
    score.scale = 2;
    score.shadow = colors::black;
    scene.set(hud, score);
    const Entity theirs = scene.createEntity("Theirs");
    scene.set(theirs, Transform{});
    scene.set(theirs, Player{{}, 2, "Bo"});
    const Entity theirHud = scene.createEntity("Their score", theirs);
    scene.set(theirHud, Transform{});
    scene.set(theirHud, score);

    Camera2D camera;
    camera.center = {100, 50};
    camera.viewport = {320, 180};
    SceneRenderOptions options;
    options.fonts = &fonts;
    options.viewer = 1;
    const auto stats = renderScene(scene, camera, renderer, textures, options);
    CHECK(stats.texts == 2);
    // The sign's letters, then the sprite above it, then the HUD (shadow, letter).
    REQUIRE(renderer.sprites.size() == 2 + 1 + 2);
    CHECK(renderer.sprites[0].texture == builtIn.texture);
    CHECK(renderer.sprites[2].texture != builtIn.texture);
    // "HI" sits on the sign's point with the bottom of its box (padding 1).
    const TextLayout signLayout = layoutText(*builtIn.data, world);
    REQUIRE(renderer.fills.size() == 1);
    CHECK(renderer.fills[0].first == Rect{160 - signLayout.size.x / 2, 90 - signLayout.size.y, signLayout.size.x,
                                          signLayout.size.y});
    // The score: 4 px from the top-right corner, twice as big, with its shadow under it.
    const SpriteDraw& shadow = renderer.sprites[3];
    const SpriteDraw& digit = renderer.sprites[4];
    CHECK(shadow.tint == colors::black);
    CHECK(shadow.dest.x == digit.dest.x + 2);
    CHECK(digit.dest.right() <= 316.0f);
    CHECK(digit.dest.top() >= 4.0f);
    CHECK(digit.dest.w == builtIn.data->find(U'9')->source.w * 2);

    // Everyone's view (playing alone) shows both HUD texts; hidden texts are not drawn.
    renderer.sprites.clear();
    options.viewer = 0;
    scene.get<Text>(sign).visible = false;
    CHECK(renderScene(scene, camera, renderer, textures, options).texts == 2);
    options.screenTexts = false;
    CHECK(renderScene(scene, camera, renderer, textures, options).texts == 0);
}

TEST_CASE("Font baking: Press Start 2P as a pixel font, and the result loads") {
    std::ifstream in(std::string(PX_SOURCE_DIR) + "/third_party/fonts/press-start-2p/PressStart2P-Regular.ttf",
                     std::ios::binary);
    REQUIRE(in);
    std::ostringstream bytes;
    bytes << in.rdbuf();
    const std::string ttf = bytes.str();
    const auto data = std::span(reinterpret_cast<const std::uint8_t*>(ttf.data()), ttf.size());
    fontbake::BakeOptions options;
    options.pixelSize = 8;
    auto baked = fontbake::bakeFont(data, options);
    REQUIRE(baked);
    CHECK(baked->familyName == "Press Start 2P");
    CHECK(baked->font.size == 8);
    CHECK(baked->font.ascent == 8);
    const project::Glyph* a = baked->font.find(U'A');
    REQUIRE(a != nullptr);
    CHECK(a->advance == 8.0f);
    CHECK(a->source.w == 7.0f);
    CHECK(a->source.h == 7.0f);
    // Pixel mode: every pixel fully on or off.
    for (std::size_t i = 3; i < baked->image.pixels.size(); i += 4) {
        const auto alpha = baked->image.pixels[i];
        REQUIRE((alpha == 0 || alpha == 255));
    }
    // The built-in font is this, as the repository has it.
    CHECK(baked->font.glyphs.size() == builtInFontData().glyphs.size());
    baked->font.texture = kTex;
    auto png = assets::encodePng(baked->image);
    REQUIRE(png);
    auto image = assets::decodeImage(png.value(), "font.png");
    REQUIRE(image);
    CHECK(project::checkFontTexture(baked->font, {image->width, image->height}, "font"));
    auto again = project::parseFont(project::serializeFont(baked->font), "font.font.json");
    REQUIRE(again);
    CHECK(again->glyphs.size() == baked->font.glyphs.size());

    // Smooth: soft edges; not a font: an error; a size out of range: an error.
    options.smooth = true;
    options.pixelSize = 24;
    auto smooth = fontbake::bakeFont(data, options);
    REQUIRE(smooth);
    CHECK(smooth->font.size == 24);
    CHECK_FALSE(fontbake::bakeFont(std::span(data.data(), 100), {}));
    options.pixelSize = 1000;
    CHECK_FALSE(fontbake::bakeFont(data, options));
}
