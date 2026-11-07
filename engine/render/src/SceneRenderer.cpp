#include "px/render/SceneRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace px::render {

using namespace px::scene;

namespace {

struct DrawItem {
    int layer = 0;
    Transform world;                  // world transform of the entity
    const Tilemap* tilemap = nullptr; // exactly one of tilemap/sprite/text is set
    const Sprite* sprite = nullptr;
    const Text* text = nullptr;
    Entity entity = kNullEntity;
};

// Rounds a screen rectangle to whole pixels so that adjacent tiles share
// edges exactly (no seams) and pixel art stays crisp.
Rect snapRect(const Rect& r) {
    const float left = std::round(r.left());
    const float top = std::round(r.top());
    return {left, top, std::round(r.right()) - left, std::round(r.bottom()) - top};
}

void drawTilemap(const Tilemap& map, const Transform& t, const Camera2D& camera, IRenderer2D& renderer,
                 TextureCache& textures, SceneRenderStats& stats) {
    if (map.width <= 0 || map.height <= 0 || map.tileSize.x <= 0.0f || map.tileSize.y <= 0.0f) return;
    const TextureHandle texture = textures.get(map.tileset);
    const bool missing = textures.isMissing(texture);

    const Rect view = camera.visibleWorldRect();
    const int x0 = std::max(0, static_cast<int>(std::floor((view.left() - t.position.x) / map.tileSize.x)));
    const int y0 = std::max(0, static_cast<int>(std::floor((view.top() - t.position.y) / map.tileSize.y)));
    const int x1 = std::min(map.width - 1, static_cast<int>(std::floor((view.right() - t.position.x) / map.tileSize.x)));
    const int y1 = std::min(map.height - 1, static_cast<int>(std::floor((view.bottom() - t.position.y) / map.tileSize.y)));

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const int tile = map.tileAt(x, y);
            if (tile < 0) continue;
            SpriteDraw draw;
            draw.texture = texture;
            if (!missing) {
                // Tileset cells are tileSize texels; see docs/project-format.md.
                const int col = tile % map.tilesetColumns;
                const int row = tile / map.tilesetColumns;
                draw.source = {static_cast<float>(col) * map.tileSize.x, static_cast<float>(row) * map.tileSize.y,
                               map.tileSize.x, map.tileSize.y};
            }
            const Rect world{t.position.x + static_cast<float>(x) * map.tileSize.x,
                             t.position.y + static_cast<float>(y) * map.tileSize.y, map.tileSize.x, map.tileSize.y};
            draw.dest = snapRect(camera.worldRectToScreen(world));
            draw.tint = map.tint;
            renderer.drawSprite(draw);
            ++stats.tiles;
        }
    }
}

void drawSprite(const Sprite& sprite, const Transform& t, const Camera2D& camera, IRenderer2D& renderer,
                TextureCache& textures, SceneRenderStats& stats) {
    const Vec2 size{sprite.size.x * std::abs(t.scale.x), sprite.size.y * std::abs(t.scale.y)};
    if (size.x <= 0.0f || size.y <= 0.0f) return;
    const Rect world{t.position.x - size.x * sprite.pivot.x, t.position.y - size.y * sprite.pivot.y, size.x, size.y};

    // Conservative bounds when rotated: a square covering any rotation around the pivot.
    Rect bounds = world;
    if (t.rotation != 0.0f) {
        const float r = length(size);
        bounds = {t.position.x - r, t.position.y - r, 2.0f * r, 2.0f * r};
    }
    if (!bounds.intersects(camera.visibleWorldRect())) {
        ++stats.culledSprites;
        return;
    }

    SpriteDraw draw;
    draw.texture = textures.get(sprite.texture);
    if (!textures.isMissing(draw.texture)) draw.source = sprite.source;
    Rect screen = camera.worldRectToScreen(world);
    if (camera.pixelSnap) {
        screen.x = std::round(screen.x);
        screen.y = std::round(screen.y);
    }
    draw.dest = screen;
    draw.rotation = t.rotation;
    draw.pivot = sprite.pivot;
    draw.flipX = sprite.flipX != (t.scale.x < 0.0f);
    draw.flipY = sprite.flipY != (t.scale.y < 0.0f);
    draw.tint = sprite.tint;
    renderer.drawSprite(draw);
    ++stats.sprites;
}

} // namespace

Camera2D cameraForScene(const Scene& scene, Vec2 viewport) {
    Camera2D camera;
    camera.viewport = viewport;
    for (const Entity e : scene.entities()) {
        if (const auto* cam = scene.tryGet<Camera>(e)) {
            camera.center = cam->position;
            camera.zoom = cam->zoom;
            break;
        }
    }
    return camera;
}

TextPlacement placeText(const Scene& scene, Entity entity, const Text& text, const TextLayout& layout,
                        const Camera2D& camera, const SceneRenderOptions& options) {
    const float scale = text.scale > 0.0f ? text.scale : 1.0f;
    const Vec2 anchor = anchorFactor(text.anchor);
    TextPlacement p;
    Vec2 point;
    if (text.screen) {
        const bool whole = options.screenArea.w <= 0.0f || options.screenArea.h <= 0.0f;
        const Rect area = whole ? Rect{0, 0, camera.viewport.x, camera.viewport.y} : options.screenArea;
        const float areaScale = whole ? 1.0f : options.screenScale;
        p.pixelScale = scale * areaScale;
        const auto* t = scene.tryGet<Transform>(entity);
        point = Vec2{area.x + anchor.x * area.w, area.y + anchor.y * area.h} +
                (t != nullptr ? t->position : Vec2{}) * areaScale;
    } else {
        p.pixelScale = scale * camera.zoom;
        point = camera.worldToScreen(scene.worldPosition(entity));
    }
    p.topLeft = point - Vec2{anchor.x * layout.size.x, anchor.y * layout.size.y} * p.pixelScale;
    return p;
}

namespace {

// Screen texts inside a player's character are that player's HUD.
bool shownTo(const Scene& scene, Entity e, int viewer) {
    if (viewer == 0) return true;
    for (Entity at = e; !at.isNull(); at = scene.parent(at)) {
        if (const auto* p = scene.tryGet<Player>(at)) return p->owner == viewer;
    }
    return true;
}

void drawText(const Scene& scene, const DrawItem& item, const Camera2D& camera, IRenderer2D& renderer,
              const SceneRenderOptions& options, SceneRenderStats& stats) {
    const Text& text = *item.text;
    const LoadedFont font = options.fonts->get(text.font);
    const TextLayout layout = layoutText(*font.data, text);
    const TextPlacement at = placeText(scene, item.entity, text, layout, camera, options);
    const Rect box{at.topLeft.x, at.topLeft.y, layout.size.x * at.pixelScale, layout.size.y * at.pixelScale};
    if (box.right() < 0.0f || box.bottom() < 0.0f || box.left() > camera.viewport.x || box.top() > camera.viewport.y) {
        return;
    }
    drawTextLayout(renderer, font, layout, text, at.topLeft, at.pixelScale);
    ++stats.texts;
}

} // namespace

SceneRenderStats renderScene(const Scene& scene, const Camera2D& camera, IRenderer2D& renderer,
                             TextureCache& textures, const SceneRenderOptions& options) {
    std::vector<DrawItem> items;
    std::vector<DrawItem> screen;
    items.reserve(scene.entityCount());
    for (const Entity e : scene.entities()) {
        const auto* map = scene.tryGet<Tilemap>(e);
        if (map != nullptr && scene.tryGet<Transform>(e) != nullptr) {
            items.push_back({map->layer, scene.worldTransform(e), map, nullptr, nullptr, e});
        }
    }
    for (const Entity e : scene.entities()) {
        const auto* sprite = scene.tryGet<Sprite>(e);
        if (sprite != nullptr && sprite->visible && scene.tryGet<Transform>(e) != nullptr) {
            items.push_back({sprite->layer, scene.worldTransform(e), nullptr, sprite, nullptr, e});
        }
        const auto* text = options.fonts != nullptr ? scene.tryGet<Text>(e) : nullptr;
        if (text == nullptr || !text->visible || text->text.empty() || scene.tryGet<Transform>(e) == nullptr) continue;
        if (text->screen) {
            if (options.screenTexts && shownTo(scene, e, options.viewer)) {
                screen.push_back({text->layer, {}, nullptr, nullptr, text, e});
            }
        } else {
            items.push_back({text->layer, {}, nullptr, nullptr, text, e});
        }
    }
    const auto byLayer = [](const DrawItem& a, const DrawItem& b) { return a.layer < b.layer; };
    std::stable_sort(items.begin(), items.end(), byLayer);
    std::stable_sort(screen.begin(), screen.end(), byLayer);

    SceneRenderStats stats;
    for (const DrawItem& item : items) {
        if (item.tilemap != nullptr) {
            drawTilemap(*item.tilemap, item.world, camera, renderer, textures, stats);
        } else if (item.sprite != nullptr) {
            drawSprite(*item.sprite, item.world, camera, renderer, textures, stats);
        } else if (item.text != nullptr) {
            drawText(scene, item, camera, renderer, options, stats);
        }
    }
    for (const DrawItem& item : screen) drawText(scene, item, camera, renderer, options, stats);
    return stats;
}

void renderCollisionOverlay(const Scene& scene, const Camera2D& camera, IRenderer2D& renderer) {
    const Rect view = camera.visibleWorldRect();
    const Color tileColor{80, 140, 255, 255};
    const Color solidColor{60, 230, 90, 255};
    const Color triggerColor{255, 220, 40, 255};

    for (const Entity e : scene.entities()) {
        if (scene.tryGet<Transform>(e) == nullptr) continue;
        const Vec2 position = scene.worldPosition(e);
        if (const auto* map = scene.tryGet<Tilemap>(e); map && map->solid) {
            for (int y = 0; y < map->height; ++y) {
                for (int x = 0; x < map->width; ++x) {
                    if (map->tileAt(x, y) < 0) continue;
                    const Rect world{position.x + static_cast<float>(x) * map->tileSize.x,
                                     position.y + static_cast<float>(y) * map->tileSize.y, map->tileSize.x,
                                     map->tileSize.y};
                    if (world.intersects(view)) renderer.drawRect(snapRect(camera.worldRectToScreen(world)), tileColor);
                }
            }
        }
        if (const auto* c = scene.tryGet<BoxCollider>(e)) {
            const Rect world = c->worldRect(position);
            if (world.intersects(view)) {
                renderer.drawRect(snapRect(camera.worldRectToScreen(world)), c->isTrigger ? triggerColor : solidColor);
            }
        }
    }
}

} // namespace px::render
