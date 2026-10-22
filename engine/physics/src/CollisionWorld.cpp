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

void CollisionWorld::addBox(const Rect& box) {
    if (!box.empty()) m_boxes.push_back(box);
}

void CollisionWorld::querySolids(const Rect& region, std::vector<Rect>& out) const {
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
                if (!grid.isSolid(x, y)) continue;
                out.push_back(Rect{grid.origin.x + static_cast<float>(x) * grid.cellSize.x,
                                   grid.origin.y + static_cast<float>(y) * grid.cellSize.y, grid.cellSize.x,
                                   grid.cellSize.y});
            }
        }
    }
    for (const auto& box : m_boxes) {
        // Inclusive test so touching solids are returned as contact candidates.
        if (box.left() <= region.right() && box.right() >= region.left() && box.top() <= region.bottom() &&
            box.bottom() >= region.top()) {
            out.push_back(box);
        }
    }
}

MoveResult CollisionWorld::moveBox(const Rect& box, Vec2 delta) const {
    MoveResult result;
    result.box = box;

    Rect swept = unite(box, box.translated(delta));
    swept = {swept.x - kSkin, swept.y - kSkin, swept.w + 2.0f * kSkin, swept.h + 2.0f * kSkin};
    std::vector<Rect> solids;
    querySolids(swept, solids);

    // X axis.
    if (delta.x != 0.0f) {
        float dx = delta.x;
        for (const Rect& s : solids) {
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
    if (delta.y != 0.0f) {
        float dy = delta.y;
        for (const Rect& s : solids) {
            const bool overlapX = result.box.right() > s.left() + kSkin && result.box.left() < s.right() - kSkin;
            if (!overlapX) continue;
            if (dy > 0.0f && s.top() >= result.box.bottom() - kSkin) {
                const float allowed = s.top() - result.box.bottom();
                if (allowed < dy) {
                    dy = allowed;
                    result.hitBottom = true;
                }
            } else if (dy < 0.0f && s.bottom() <= result.box.top() + kSkin) {
                const float allowed = s.bottom() - result.box.top();
                if (allowed > dy) {
                    dy = allowed;
                    result.hitTop = true;
                }
            }
        }
        result.box.y += dy;
        result.moved.y = dy;
    }

    return result;
}

bool CollisionWorld::overlapsSolid(const Rect& box) const {
    std::vector<Rect> solids;
    querySolids(box, solids);
    return std::any_of(solids.begin(), solids.end(), [&](const Rect& s) { return s.intersects(box); });
}

} // namespace px::physics
