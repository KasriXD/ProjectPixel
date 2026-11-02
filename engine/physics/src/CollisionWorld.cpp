#include "px/physics/CollisionWorld.hpp"

#include <algorithm>
#include <cmath>

namespace px::physics {

void CollisionWorld::clear() {
    m_grids.clear();
    m_boxes.clear();
}

void CollisionWorld::addGrid(SolidGrid grid) {
    if (grid.width <= 0 || grid.height <= 0 || grid.cellSize.x <= 0.0f || grid.cellSize.y <= 0.0f) return;
    grid.solid.resize(static_cast<std::size_t>(grid.width) * static_cast<std::size_t>(grid.height), 0);
    m_grids.push_back(std::move(grid));
}

void CollisionWorld::addBox(const Rect& box, SolidOwner owner, Shape shape) {
    if (!box.empty() && shape != Shape::Empty) m_boxes.push_back(Solid{box, owner, shape});
}

void CollisionWorld::clearBoxes() { m_boxes.clear(); }

void CollisionWorld::querySolids(const Rect& region, std::vector<Solid>& out) const {
    for (const auto& grid : m_grids) {
        const float localLeft = (region.left() - grid.origin.x) / grid.cellSize.x;
        const float localRight = (region.right() - grid.origin.x) / grid.cellSize.x;
        const float localTop = (region.top() - grid.origin.y) / grid.cellSize.y;
        const float localBottom = (region.bottom() - grid.origin.y) / grid.cellSize.y;
        if (localRight < 0.0f || localBottom < 0.0f || localLeft >= static_cast<float>(grid.width) ||
            localTop >= static_cast<float>(grid.height)) {
            continue;
        }
        const int x0 = std::max(0, static_cast<int>(std::floor(localLeft)));
        const int x1 = std::min(grid.width - 1, static_cast<int>(std::floor(localRight)));
        const int y0 = std::max(0, static_cast<int>(std::floor(localTop)));
        const int y1 = std::min(grid.height - 1, static_cast<int>(std::floor(localBottom)));
        for (int y = y0; y <= y1; ++y) {
            for (int x = x0; x <= x1; ++x) {
                const Shape shape = grid.shapeAt(x, y);
                if (shape == Shape::Empty) continue;
                out.push_back(Solid{Rect{grid.origin.x + static_cast<float>(x) * grid.cellSize.x,
                                         grid.origin.y + static_cast<float>(y) * grid.cellSize.y, grid.cellSize.x,
                                         grid.cellSize.y},
                                    kNoOwner, shape});
            }
        }
    }
    for (const auto& solid : m_boxes) {
        // Inclusive test so touching solids are returned as contact candidates.
        const Rect& box = solid.rect;
        if (box.left() <= region.right() && box.right() >= region.left() && box.top() <= region.bottom() &&
            box.bottom() >= region.top()) {
            out.push_back(solid);
        }
    }
}

namespace {

// The height (Y, pointing down) of a slope's surface under `box`: the
// highest point of the slope within the box's horizontal extent, where the
// box's lower corner rests.
float slopeSurface(const Solid& s, const Rect& box) {
    const Rect& r = s.rect;
    if (s.shape == Shape::SlopeUpRight) {
        const float x = std::clamp(box.right(), r.left(), r.right());
        return r.bottom() - (x - r.left()) * (r.h / r.w);
    }
    const float x = std::clamp(box.left(), r.left(), r.right());
    return r.top() + (x - r.left()) * (r.h / r.w);
}

bool overlapsX(const Rect& box, const Rect& s) {
    return box.right() > s.left() + CollisionWorld::kSkin && box.left() < s.right() - CollisionWorld::kSkin;
}

// The highest floor under `box` no further down than `limit`, as the
// distance the box may move down (negative: a slope lifts it). `startBottom`
// is where the box's bottom was before this move (one-way floors only catch
// boxes that were above them); `lift` is how far a slope may push it up (how
// far it moved sideways this step).
struct Floor {
    float allowed = 0.0f;
    bool found = false;
    SolidOwner owner = kNoOwner;
};

Floor findFloor(const std::vector<Solid>& solids, const Rect& box, float startBottom, float limit, float lift) {
    Floor floor;
    const float bottom = box.bottom();
    for (const Solid& solid : solids) {
        const Rect& s = solid.rect;
        if (!overlapsX(box, s)) continue;
        float top = 0.0f;
        switch (solid.shape) {
            case Shape::Empty: continue;
            case Shape::Solid:
                if (s.top() < bottom - CollisionWorld::kSkin) continue; // beside or above: not a floor
                top = s.top();
                break;
            case Shape::OneWay:
                if (s.top() < startBottom - CollisionWorld::kSkin || s.top() < bottom - CollisionWorld::kSkin) continue;
                top = s.top();
                break;
            case Shape::SlopeUpRight:
            case Shape::SlopeUpLeft:
                top = slopeSurface(solid, box);
                if (bottom - top > lift + CollisionWorld::kSkin) continue; // deep inside: not walking onto it
                break;
        }
        const float allowed = top - bottom;
        if (allowed > limit) continue;
        if (!floor.found || allowed < floor.allowed) {
            floor.allowed = allowed;
            floor.owner = solid.owner;
            floor.found = true;
        }
    }
    return floor;
}

} // namespace

MoveResult CollisionWorld::moveBox(const Rect& box, Vec2 delta, float snapDown) const {
    MoveResult result;
    result.box = box;

    Rect swept = unite(box, box.translated(delta));
    swept = {swept.x - kSkin, swept.y - kSkin, swept.w + 2.0f * kSkin, swept.h + 2.0f * kSkin + std::max(0.0f, snapDown)};
    // Slopes may lift the box by up to the sideways distance: look that far up too.
    const float lift = std::abs(delta.x);
    swept.y -= lift;
    swept.h += lift;
    std::vector<Solid> solids;
    querySolids(swept, solids);

    // X axis: only fully solid shapes block sideways movement.
    if (delta.x != 0.0f) {
        float dx = delta.x;
        for (const Solid& solid : solids) {
            if (solid.shape != Shape::Solid) continue;
            const Rect& s = solid.rect;
            const bool overlapY = result.box.bottom() > s.top() + kSkin && result.box.top() < s.bottom() - kSkin;
            if (!overlapY) continue;
            if (dx > 0.0f && s.left() >= result.box.right() - kSkin) {
                const float allowed = s.left() - result.box.right();
                if (allowed < dx) {
                    dx = allowed;
                    result.hitRight = true;
                }
            } else if (dx < 0.0f && s.right() <= result.box.left() + kSkin) {
                const float allowed = s.right() - result.box.left();
                if (allowed > dx) {
                    dx = allowed;
                    result.hitLeft = true;
                }
            }
        }
        result.box.x += dx;
        result.moved.x = dx;
    }

    // Y axis, using the post-X position.
    float dy = delta.y;
    if (dy < 0.0f) {
        // Up: only solid ceilings stop it.
        for (const Solid& solid : solids) {
            if (solid.shape != Shape::Solid) continue;
            const Rect& s = solid.rect;
            if (!overlapsX(result.box, s)) continue;
            if (s.bottom() <= result.box.top() + kSkin) {
                const float allowed = s.bottom() - result.box.top();
                if (allowed > dy) {
                    dy = allowed;
                    result.hitTop = true;
                }
            }
        }
    } else if (dy > 0.0f || snapDown > 0.0f) {
        // Down: floors, slopes lifting the box, then snapping.
        const Floor floor = findFloor(solids, result.box, box.bottom(), dy, std::abs(result.moved.x));
        if (floor.found) {
            dy = floor.allowed;
            result.hitBottom = true;
            result.groundOwner = floor.owner;
        } else if (snapDown > 0.0f) {
            const Floor below = findFloor(solids, result.box, box.bottom(), dy + snapDown, 0.0f);
            if (below.found) {
                dy = below.allowed;
                result.hitBottom = true;
                result.groundOwner = below.owner;
            }
        }
    }
    result.box.y += dy;
    result.moved.y = dy;
    return result;
}

bool CollisionWorld::overlapsSolid(const Rect& box) const {
    std::vector<Solid> solids;
    querySolids(box, solids);
    return std::any_of(solids.begin(), solids.end(), [&](const Solid& s) { return s.rect.intersects(box); });
}

} // namespace px::physics
