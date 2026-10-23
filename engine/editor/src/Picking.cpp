#include "px/editor/Picking.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <numbers>

namespace px::editor {

using namespace px::scene;

namespace {

Rect spriteRect(const Sprite& sprite, const Transform& world) {
    const Vec2 size{sprite.size.x * std::abs(world.scale.x), sprite.size.y * std::abs(world.scale.y)};
    return {world.position.x - size.x * sprite.pivot.x, world.position.y - size.y * sprite.pivot.y, size.x, size.y};
}

// Whether a world point hits a sprite, honouring rotation around the position.
bool spriteContains(const Sprite& sprite, const Transform& world, Vec2 point) {
    Vec2 p = point;
    if (world.rotation != 0.0f) {
        const double r = -static_cast<double>(world.rotation) * std::numbers::pi / 180.0;
        const auto c = static_cast<float>(std::cos(r));
        const auto s = static_cast<float>(std::sin(r));
        const Vec2 d = point - world.position;
        p = world.position + Vec2{d.x * c - d.y * s, d.x * s + d.y * c};
    }
    return spriteRect(sprite, world).contains(p);
}

Rect handleRect(Vec2 position, float handleSize) {
    return Rect::fromCenter(position, {handleSize, handleSize});
}

} // namespace

Rect entityBounds(const Scene& scene, Entity entity, float handleSize) {
    if (scene.tryGet<Transform>(entity) == nullptr) return {};
    const Transform world = scene.worldTransform(entity);
    bool any = false;
    Rect bounds;
    const auto add = [&](const Rect& r) {
        bounds = any ? unite(bounds, r) : r;
        any = true;
    };
    if (const auto* sprite = scene.tryGet<Sprite>(entity)) {
        Rect r = spriteRect(*sprite, world);
        if (world.rotation != 0.0f) {
            const float radius = length(r.size());
            r = Rect::fromCenter(world.position, {2.0f * radius, 2.0f * radius});
        }
        add(r);
    }
    if (const auto* map = scene.tryGet<Tilemap>(entity)) add(map->worldBounds(world.position));
    if (const auto* collider = scene.tryGet<BoxCollider>(entity)) add(collider->worldRect(world.position));
    if (!any) add(handleRect(world.position, handleSize));
    return bounds;
}

std::vector<Entity> pickAll(const Scene& scene, Vec2 point, float handleSize) {
    struct Hit {
        Entity entity;
        int tier;  // 2 = sprite, 1 = collider/handle, 0 = tile
        int layer;
        std::size_t order;
    };
    std::vector<Hit> hits;
    const auto& entities = scene.entities();
    for (std::size_t i = 0; i < entities.size(); ++i) {
        const Entity e = entities[i];
        if (scene.tryGet<Transform>(e) == nullptr) continue;
        const Transform world = scene.worldTransform(e);
        const auto* sprite = scene.tryGet<Sprite>(e);
        if (sprite != nullptr && spriteContains(*sprite, world, point)) {
            hits.push_back({e, 2, sprite->layer, i});
            continue;
        }
        if (const auto* collider = scene.tryGet<BoxCollider>(e); collider && collider->worldRect(world.position).contains(point)) {
            hits.push_back({e, 1, INT_MAX, i});
            continue;
        }
        const auto* map = scene.tryGet<Tilemap>(e);
        if (map != nullptr) {
            const TileCoord tile = tileAt(scene, e, point);
            if (tile.inside && map->tileAt(tile.x, tile.y) >= 0) hits.push_back({e, 0, map->layer, i});
            continue;
        }
        if (sprite == nullptr && handleRect(world.position, handleSize).contains(point)) {
            hits.push_back({e, 1, INT_MAX, i});
        }
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.tier != b.tier) return a.tier > b.tier;
        if (a.layer != b.layer) return a.layer > b.layer;
        return a.order > b.order;
    });
    std::vector<Entity> out;
    out.reserve(hits.size());
    for (const Hit& h : hits) out.push_back(h.entity);
    return out;
}

Entity pick(const Scene& scene, Vec2 point, float handleSize) {
    const auto hits = pickAll(scene, point, handleSize);
    return hits.empty() ? kNullEntity : hits.front();
}

std::vector<Entity> entitiesInside(const Scene& scene, const Rect& rect, float handleSize) {
    std::vector<Entity> out;
    for (const Entity e : scene.entities()) {
        const Rect b = entityBounds(scene, e, handleSize);
        if (b.empty()) continue;
        if (b.left() >= rect.left() && b.right() <= rect.right() && b.top() >= rect.top() &&
            b.bottom() <= rect.bottom()) {
            out.push_back(e);
        }
    }
    return out;
}

TileCoord tileAt(const Scene& scene, Entity tilemap, Vec2 point) {
    TileCoord out;
    const auto* map = scene.tryGet<Tilemap>(tilemap);
    if (map == nullptr || map->tileSize.x <= 0.0f || map->tileSize.y <= 0.0f) return out;
    const Vec2 origin = scene.worldPosition(tilemap);
    out.x = static_cast<int>(std::floor((point.x - origin.x) / map->tileSize.x));
    out.y = static_cast<int>(std::floor((point.y - origin.y) / map->tileSize.y));
    out.inside = out.x >= 0 && out.y >= 0 && out.x < map->width && out.y < map->height;
    return out;
}

Vec2 snapToGrid(Vec2 v, float step) {
    if (step <= 0.0f) return v;
    return {std::round(v.x / step) * step, std::round(v.y / step) * step};
}

} // namespace px::editor
