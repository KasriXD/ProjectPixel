#include <doctest.h>

#include "px/physics/CollisionWorld.hpp"

using namespace px;
using namespace px::physics;

namespace {

// 10x6 grid of 16px cells with a floor on row 5 and a wall at column 6 (rows 2-4).
CollisionWorld makeWorld() {
    SolidGrid grid;
    grid.width = 10;
    grid.height = 6;
    grid.cellSize = {16, 16};
    grid.solid.assign(60, 0);
    for (int x = 0; x < 10; ++x) grid.solid[5 * 10 + x] = 1;
    for (int y = 2; y <= 4; ++y) grid.solid[y * 10 + 6] = 1;
    CollisionWorld world;
    world.addGrid(grid);
    return world;
}

} // namespace

TEST_CASE("Falling box lands exactly on the floor") {
    const CollisionWorld world = makeWorld();
    const Rect box{20, 40, 12, 14};
    const MoveResult r = world.moveBox(box, {0, 100});
    CHECK(r.hitBottom);
    CHECK_FALSE(r.hitTop);
    CHECK(r.box.bottom() == doctest::Approx(80.0f)); // floor top = 5 * 16
    CHECK(r.moved.y == doctest::Approx(80.0f - 54.0f));
}

TEST_CASE("Walking along the floor is not blocked by floor tile seams") {
    const CollisionWorld world = makeWorld();
    const Rect box{20, 80 - 14, 12, 14}; // resting on the floor
    const MoveResult r = world.moveBox(box, {30, 0.25f});
    CHECK_FALSE(r.hitRight);
    CHECK(r.moved.x == doctest::Approx(30.0f));
    CHECK(r.hitBottom); // still touching the ground
    CHECK(r.moved.y == doctest::Approx(0.0f));
}

TEST_CASE("Walls stop horizontal movement in both directions") {
    const CollisionWorld world = makeWorld();
    const Rect box{70, 80 - 14, 12, 14}; // right edge at 82; wall starts at 96
    const MoveResult right = world.moveBox(box, {50, 0});
    CHECK(right.hitRight);
    CHECK(right.box.right() == doctest::Approx(96.0f));

    const Rect beyond{120, 80 - 14, 12, 14}; // left edge at 120; wall ends at 112
    const MoveResult left = world.moveBox(beyond, {-50, 0});
    CHECK(left.hitLeft);
    CHECK(left.box.left() == doctest::Approx(112.0f));
}

TEST_CASE("Ceilings stop upward movement") {
    CollisionWorld world;
    world.addBox({0, 0, 100, 10});
    const MoveResult r = world.moveBox({10, 30, 10, 10}, {0, -50});
    CHECK(r.hitTop);
    CHECK(r.box.top() == doctest::Approx(10.0f));
}

TEST_CASE("Fast movement never tunnels through thin geometry") {
    CollisionWorld world;
    world.addBox({100, -50, 2, 200}); // 2px-thick wall
    const MoveResult r = world.moveBox({0, 0, 10, 10}, {5000, 0});
    CHECK(r.hitRight);
    CHECK(r.box.right() == doctest::Approx(100.0f));
}

TEST_CASE("Boxes overlapping geometry can move out of it") {
    CollisionWorld world;
    world.addBox({0, 0, 20, 20});
    // Starts inside the solid: it must not be trapped.
    const MoveResult r = world.moveBox({5, 5, 10, 10}, {40, 0});
    CHECK(r.moved.x == doctest::Approx(40.0f));
    CHECK(world.overlapsSolid({5, 5, 10, 10}));
    CHECK_FALSE(world.overlapsSolid({30, 30, 5, 5}));
}

TEST_CASE("Out-of-grid cells are empty and diagonal moves resolve per axis") {
    const CollisionWorld world = makeWorld();
    // Falling past the left edge of the grid: nothing to land on.
    const MoveResult off = world.moveBox({-40, 0, 10, 10}, {0, 200});
    CHECK_FALSE(off.hitBottom);
    // Diagonal into floor: X fully applied, Y clamped.
    const MoveResult diag = world.moveBox({16, 60, 10, 10}, {8, 20});
    CHECK(diag.moved.x == doctest::Approx(8.0f));
    CHECK(diag.hitBottom);
    CHECK(diag.box.bottom() == doctest::Approx(80.0f));
}
