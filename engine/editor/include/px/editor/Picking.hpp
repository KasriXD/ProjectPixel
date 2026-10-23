#pragma once

#include "px/core/Math.hpp"
#include "px/scene/Scene.hpp"

#include <vector>

// Geometry queries for the editor viewport: what is under the mouse, and
// the outline of an entity. Works on world coordinates only, so it is the
// same for any camera and is unit tested without graphics.
namespace px::editor {

// World-space bounds of what the entity draws or collides with: its sprite
// (rotation included, conservatively), tilemap and collider. Entities with a
// Transform but nothing visible get a square of `handleSize` around their
// position, so they can still be clicked. Empty for entities without a Transform.
[[nodiscard]] Rect entityBounds(const scene::Scene& scene, scene::Entity entity, float handleSize = 8.0f);

// Entities under a world point, topmost first: sprites by layer (higher
// first, later in hierarchy order first within a layer), then colliders and
// handles of entities without sprites, then tilemaps where the tile under
// the point is not empty. Group entities (no Transform) are never hit.
[[nodiscard]] std::vector<scene::Entity> pickAll(const scene::Scene& scene, Vec2 point, float handleSize = 8.0f);
[[nodiscard]] scene::Entity pick(const scene::Scene& scene, Vec2 point, float handleSize = 8.0f);

// Entities whose bounds lie entirely inside `rect` (rectangle selection).
[[nodiscard]] std::vector<scene::Entity> entitiesInside(const scene::Scene& scene, const Rect& rect,
                                                        float handleSize = 8.0f);

struct TileCoord {
    int x = 0;
    int y = 0;
    bool inside = false; // within the map's width and height
};
// The tile cell of a tilemap entity at a world point.
[[nodiscard]] TileCoord tileAt(const scene::Scene& scene, scene::Entity tilemap, Vec2 point);

// Rounds v to the nearest multiple of `step` (step <= 0: unchanged).
[[nodiscard]] Vec2 snapToGrid(Vec2 v, float step);

} // namespace px::editor
