#pragma once

#include "px/core/Math.hpp"

#include <cstdint>
#include <vector>

// Collision for kinematic, axis-aligned movement: solid tile grids and solid
// boxes, swept movement with per-axis contact flags. This module knows
// nothing about scenes or components; the runtime feeds it geometry.
// Rigid-body simulation (Box2D) is planned behind a separate interface; see
// docs/adr/0004-physics.md.
namespace px::physics {

struct SolidGrid {
    Vec2 origin{};                  // world position of cell (0, 0)'s top-left corner
    Vec2 cellSize{16.0f, 16.0f};
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> solid; // row-major, non-zero = solid

    [[nodiscard]] bool isSolid(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return false;
        return solid[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] != 0;
    }
};

// Identifies who owns a solid box (e.g. an entity), so movement results can
// say what a body is standing on. kNoOwner is used for tile grids.
using SolidOwner = std::uint32_t;
inline constexpr SolidOwner kNoOwner = 0;

struct Solid {
    Rect rect;
    SolidOwner owner = kNoOwner;
};

struct MoveResult {
    Rect box;            // final box
    Vec2 moved{};        // actual displacement applied
    bool hitLeft = false;
    bool hitRight = false;
    bool hitTop = false;    // ceiling (moving up, Y-down world)
    bool hitBottom = false; // ground (moving down)
    SolidOwner groundOwner = kNoOwner; // owner of the solid that stopped downward movement
};

class CollisionWorld {
public:
    // Distance used to treat touching surfaces as contact and to absorb
    // floating-point error. Solids already overlapping a box by more than
    // this do not block it, so bodies can always move out of geometry.
    static constexpr float kSkin = 1.0e-3f;

    void clear();
    void addGrid(SolidGrid grid);
    void addBox(const Rect& box, SolidOwner owner = kNoOwner);
    // Removes all boxes but keeps grids (boxes may move every tick).
    void clearBoxes();

    // Moves `box` by `delta`, first along X then along Y, stopping at solids.
    // Never tunnels: every solid in the swept region is considered.
    [[nodiscard]] MoveResult moveBox(const Rect& box, Vec2 delta) const;

    // True if `box` strictly overlaps any solid.
    [[nodiscard]] bool overlapsSolid(const Rect& box) const;

    // Appends all solids overlapping or touching `region`.
    void querySolids(const Rect& region, std::vector<Solid>& out) const;

    [[nodiscard]] const std::vector<SolidGrid>& grids() const { return m_grids; }
    [[nodiscard]] const std::vector<Solid>& boxes() const { return m_boxes; }

private:
    std::vector<SolidGrid> m_grids;
    std::vector<Solid> m_boxes;
};

} // namespace px::physics
