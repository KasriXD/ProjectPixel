#include "px/render/SceneRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace px::render {

using namespace px::scene;

namespace {

struct DrawItem {
    int layer = 0;
    bool isTilemap = false;
    Entity entity;
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

SceneRenderStats renderScene(const Scene& scene, const Camera2D& camera, IRenderer2D& renderer,
                             TextureCache& textures) {
    std::vector<DrawItem> items;
    items.reserve(scene.entityCount());
    for (const Entity e : scene.entities()) {
        if (const auto* map = scene.tryGet<Tilemap>(e); map && scene.tryGet<Transform>(e)) {
            items.push_back({map->layer, true, e});
        }
    }
    for (const Entity e : scene.entities()) {
        if (const auto* sprite = scene.tryGet<Sprite>(e); sprite && sprite->visible && scene.tryGet<Transform>(e)) {
            items.push_back({sprite->layer, false, e});
        }
    }
    std::stable_sort(items.begin(), items.end(), [](const DrawItem& a, const DrawItem& b) { return a.layer < b.layer; });

    SceneRenderStats stats;
    for (const DrawItem& item : items) {
        const auto& t = *scene.tryGet<Transform>(item.entity);
        if (item.isTilemap) {
            drawTilemap(*scene.tryGet<Tilemap>(item.entity), t, camera, renderer, textures, stats);
        } else {
            drawSprite(*scene.tryGet<Sprite>(item.entity), t, camera, renderer, textures, stats);
        }
    }
    return stats;
}

void renderCollisionOverlay(const Scene& scene, const Camera2D& camera, IRenderer2D& renderer) {
    const Rect view = camera.visibleWorldRect();
    const Color tileColor{80, 140, 255, 255};
    const Color solidColor{60, 230, 90, 255};
    const Color triggerColor{255, 220, 40, 255};

    for (const Entity e : scene.entities()) {
        const auto* t = scene.tryGet<Transform>(e);
        if (t == nullptr) continue;
        if (const auto* map = scene.tryGet<Tilemap>(e); map && map->solid) {
            for (int y = 0; y < map->height; ++y) {
                for (int x = 0; x < map->width; ++x) {
                    if (map->tileAt(x, y) < 0) continue;
                    const Rect world{t->position.x + static_cast<float>(x) * map->tileSize.x,
                                     t->position.y + static_cast<float>(y) * map->tileSize.y, map->tileSize.x,
                                     map->tileSize.y};
                    if (world.intersects(view)) renderer.drawRect(snapRect(camera.worldRectToScreen(world)), tileColor);
                }
            }
        }
        if (const auto* c = scene.tryGet<BoxCollider>(e)) {
            const Rect world = c->worldRect(t->position);
            if (world.intersects(view)) {
                renderer.drawRect(snapRect(camera.worldRectToScreen(world)), c->isTrigger ? triggerColor : solidColor);
            }
        }
    }
}

} // namespace px::render
