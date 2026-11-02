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

// --- One-way floors and slopes -------------------------------------------------------------

namespace {

// Row 5 is solid ground. Row 2 has one-way tiles at columns 1-3. Row 4 has a
// '/' slope at column 5 and a '\' slope at column 8, with solid tiles at 6-7
// between them (a little hill), so the slopes' high sides touch solid ground.
CollisionWorld makeShapes() {
    SolidGrid grid;
    grid.width = 12;
    grid.height = 6;
    grid.cellSize = {16, 16};
    grid.solid.assign(72, 0);
    const auto set = [&](int x, int y, Shape s) { grid.solid[static_cast<std::size_t>(y * 12 + x)] = static_cast<std::uint8_t>(s); };
    for (int x = 0; x < 12; ++x) set(x, 5, Shape::Solid);
    for (int x = 1; x <= 3; ++x) set(x, 2, Shape::OneWay);
    set(5, 4, Shape::SlopeUpRight);
    set(6, 4, Shape::Solid);
    set(7, 4, Shape::Solid);
    set(8, 4, Shape::SlopeUpLeft);
    CollisionWorld world;
    world.addGrid(grid);
    return world;
}

// Walks a box resting on the ground `steps` times by `dx`, with a little
// gravity and the snapping a grounded body uses.
Rect walk(const CollisionWorld& world, Rect box, float dx, int steps) {
    for (int i = 0; i < steps; ++i) box = world.moveBox(box, {dx, 0.25f}, std::abs(dx) + 1.0f).box;
    return box;
}

} // namespace

TEST_CASE("One-way floors: land on them, jump up through them, walk through them") {
    const CollisionWorld world = makeShapes();
    // Falling from above: lands on the top (row 2 top = 32).
    const MoveResult land = world.moveBox({20, 0, 10, 14}, {0, 40});
    CHECK(land.hitBottom);
    CHECK(land.box.bottom() == doctest::Approx(32.0f));
    // Jumping from below: passes through.
    const MoveResult up = world.moveBox({20, 50, 10, 14}, {0, -40});
    CHECK_FALSE(up.hitTop);
    CHECK(up.box.top() == doctest::Approx(10.0f));
    // Halfway through (bottom below the top): keeps rising, then does not snag while falling back.
    const MoveResult inside = world.moveBox({20, 24, 10, 14}, {0, 5});
    CHECK_FALSE(inside.hitBottom);
    // Sideways at its height: not a wall.
    const MoveResult side = world.moveBox({70, 26, 10, 14}, {-60, 0});
    CHECK_FALSE(side.hitLeft);
    CHECK(side.box.left() == doctest::Approx(10.0f));
    // A one-way box (a moving platform) behaves the same.
    CollisionWorld boxes;
    boxes.addBox({0, 50, 40, 8}, 7, Shape::OneWay);
    const MoveResult onBox = boxes.moveBox({10, 30, 10, 10}, {0, 20});
    CHECK(onBox.hitBottom);
    CHECK(onBox.groundOwner == 7);
    CHECK(onBox.box.bottom() == doctest::Approx(50.0f));
    CHECK_FALSE(boxes.moveBox({10, 60, 10, 10}, {0, -30}).hitTop);
}

TEST_CASE("Slopes: walking up and down a hill stays on the surface") {
    const CollisionWorld world = makeShapes();
    // Start on the ground (top 80) left of the hill, walk right over it.
    Rect box{50, 66, 10, 14};
    box = walk(world, box, 1.5f, 10); // right edge reaches x = 75, 11 into the '/' tile (80..96)
    CHECK(box.bottom() == doctest::Approx(80.0f).epsilon(0.01));
    box = walk(world, box, 1.5f, 10); // right edge at 90: the surface under it is 80 - 10
    CHECK(box.bottom() == doctest::Approx(70.0f).epsilon(0.02));
    box = walk(world, box, 1.5f, 10); // right edge at 105: on the solid top (64)
    CHECK(box.bottom() == doctest::Approx(64.0f).epsilon(0.02));
    box = walk(world, box, 1.5f, 30); // over the top and down the '\' slope (128..144)
    CHECK(box.left() > 130.0f);
    CHECK(box.bottom() > 64.0f);
    CHECK(box.bottom() < 80.0f);
    const float surface = 64.0f + (box.left() - 128.0f);
    CHECK(box.bottom() == doctest::Approx(surface).epsilon(0.02));
    box = walk(world, box, 1.5f, 20); // and back on the ground
    CHECK(box.bottom() == doctest::Approx(80.0f).epsilon(0.01));
}

TEST_CASE("Slopes: landing on one, and no pop-up from deep inside") {
    const CollisionWorld world = makeShapes();
    // Falling onto the middle of the '/' slope: the box's right edge at 88 -> surface 72.
    const MoveResult r = world.moveBox({78, 30, 10, 14}, {0, 60});
    CHECK(r.hitBottom);
    CHECK(r.box.bottom() == doctest::Approx(72.0f));
    // A box inside the slope tile (not walking onto it) is not lifted.
    const MoveResult inside = world.moveBox({81, 70, 4, 8}, {0, 1});
    CHECK(inside.box.bottom() == doctest::Approx(79.0f));
    // Walking without snapping (in the air) does not follow the slope down.
    const MoveResult air = world.moveBox({130, 40, 10, 14}, {3, 0.1f});
    CHECK_FALSE(air.hitBottom);
}
