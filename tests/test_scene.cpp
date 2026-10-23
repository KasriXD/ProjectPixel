#include <doctest.h>

#include "px/scene/Scene.hpp"

#include <string>
#include <utility>
#include <vector>

using namespace px;
using namespace px::scene;

namespace {
struct Health {
    int value = 100;
};
struct Tag {};
} // namespace

TEST_CASE("Registry creates, destroys and recycles entities with generations") {
    Registry reg;
    const Entity a = reg.create();
    const Entity b = reg.create();
    CHECK(reg.isAlive(a));
    CHECK(reg.isAlive(b));
    CHECK(reg.aliveCount() == 2);

    reg.destroy(a);
    CHECK_FALSE(reg.isAlive(a));
    CHECK(reg.aliveCount() == 1);

    const Entity c = reg.create(); // reuses a's slot
    CHECK(c.index == a.index);
    CHECK(c.generation != a.generation);
    CHECK_FALSE(reg.isAlive(a)); // the old handle stays dead
    CHECK(reg.isAlive(c));
    CHECK_FALSE(reg.isAlive(kNullEntity));
}

TEST_CASE("Registry components: set, get, remove, destroy") {
    Registry reg;
    const Entity e = reg.create();
    CHECK_FALSE(reg.has<Health>(e));
    reg.set(e, Health{50});
    REQUIRE(reg.has<Health>(e));
    CHECK(reg.get<Health>(e).value == 50);
    reg.set(e, Health{75}); // replace
    CHECK(reg.get<Health>(e).value == 75);
    CHECK(reg.count<Health>() == 1);

    reg.remove<Health>(e);
    CHECK_FALSE(reg.has<Health>(e));

    reg.set(e, Health{});
    reg.set(e, Tag{});
    reg.destroy(e);
    CHECK(reg.count<Health>() == 0);
    CHECK(reg.count<Tag>() == 0);
    CHECK(reg.tryGet<Health>(e) == nullptr);
}

TEST_CASE("Registry::each visits entities with all requested components") {
    Registry reg;
    std::vector<Entity> withBoth;
    for (int i = 0; i < 10; ++i) {
        const Entity e = reg.create();
        reg.set(e, Health{i});
        if (i % 2 == 0) {
            reg.set(e, Tag{});
            withBoth.push_back(e);
        }
    }
    int visited = 0;
    int sum = 0;
    reg.each<Health, Tag>([&](Entity, Health& h, Tag&) {
        ++visited;
        sum += h.value;
    });
    CHECK(visited == 5);
    CHECK(sum == 0 + 2 + 4 + 6 + 8);

    // Destroying the current entity during iteration is allowed.
    int seen = 0;
    reg.each<Health>([&](Entity e, Health&) {
        ++seen;
        reg.destroy(e);
    });
    CHECK(seen == 10);
    CHECK(reg.aliveCount() == 0);
}

TEST_CASE("Registry copies are deep and independent") {
    Registry a;
    const Entity e = a.create();
    a.set(e, Health{1});
    Registry b = a;
    b.get<Health>(e).value = 2;
    CHECK(a.get<Health>(e).value == 1);
    CHECK(b.get<Health>(e).value == 2);
}

TEST_CASE("Scene entity identity, lookup and ordering") {
    Scene scene;
    const Entity player = scene.createEntity("Player");
    const Entity coin = scene.createEntity("Coin");
    const auto* info = scene.tryGet<EntityInfo>(player);
    REQUIRE(info != nullptr);
    CHECK(info->name == "Player");
    CHECK(scene.findById(info->id) == player);
    CHECK(scene.findByName("Coin") == coin);
    CHECK(scene.findByName("Nobody").isNull());

    const Guid fixed = *Guid::parse("11111111-2222-4333-8444-555555555555");
    auto created = scene.createEntityWithId(fixed, "Fixed");
    REQUIRE(created.ok());
    CHECK_FALSE(scene.createEntityWithId(fixed, "Duplicate").ok());
    CHECK_FALSE(scene.createEntityWithId(Guid{}, "Nil").ok());

    REQUIRE(scene.entities().size() == 3);
    CHECK(scene.entities()[0] == player);
    scene.destroyEntity(coin);
    REQUIRE(scene.entities().size() == 2);
    CHECK(scene.entities()[1] == created.value());
    CHECK(scene.findByName("Coin").isNull());
}

TEST_CASE("Tilemap helpers") {
    Tilemap map;
    map.width = 3;
    map.height = 2;
    map.tiles = {0, -1, 2, 3, 4, -1};
    CHECK(map.tileAt(0, 0) == 0);
    CHECK(map.tileAt(1, 0) == -1);
    CHECK(map.tileAt(1, 1) == 4);
    CHECK(map.tileAt(-1, 0) == -1);
    CHECK(map.tileAt(3, 0) == -1);
    CHECK(map.worldBounds({10, 20}) == Rect{10, 20, 48, 32});
}

namespace {
std::vector<std::string> names(const Scene& scene, const std::vector<Entity>& entities) {
    std::vector<std::string> out;
    for (const Entity e : entities) out.push_back(scene.get<EntityInfo>(e).name);
    return out;
}
using Names = std::vector<std::string>;
} // namespace

TEST_CASE("Scene hierarchy keeps depth-first order") {
    Scene scene;
    const Entity a = scene.createEntity("A");
    const Entity b = scene.createEntity("B");
    const Entity a1 = scene.createEntity("A1", a);
    const Entity a2 = scene.createEntity("A2", a);
    const Entity a1x = scene.createEntity("A1x", a1);
    CHECK(names(scene, scene.entities()) == Names{"A", "A1", "A1x", "A2", "B"});
    CHECK(scene.parent(a1x) == a1);
    CHECK(scene.parent(a).isNull());
    CHECK(scene.depth(a1x) == 2);
    CHECK(scene.isAncestor(a, a1x));
    CHECK_FALSE(scene.isAncestor(a1x, a));
    CHECK(names(scene, scene.children(a)) == Names{"A1", "A2"});
    CHECK(names(scene, scene.children(kNullEntity)) == Names{"A", "B"});
    CHECK(scene.hasChildren(a1));
    CHECK_FALSE(scene.hasChildren(a2));
    CHECK(names(scene, scene.subtree(a1)) == Names{"A1", "A1x"});

    // Moving a subtree carries its descendants; `before` controls sibling order.
    REQUIRE(scene.setParent(a1, b).ok());
    CHECK(names(scene, scene.entities()) == Names{"A", "A2", "B", "A1", "A1x"});
    REQUIRE(scene.setParent(a2, b, a1).ok());
    CHECK(names(scene, scene.entities()) == Names{"A", "B", "A2", "A1", "A1x"});
    REQUIRE(scene.setParent(b, kNullEntity, a).ok()); // reorder roots
    CHECK(names(scene, scene.entities()) == Names{"B", "A2", "A1", "A1x", "A"});

    // Cycles and bad insertion points are refused without changes.
    CHECK_FALSE(scene.setParent(b, a1x).ok());
    CHECK_FALSE(scene.setParent(b, b).ok());
    CHECK_FALSE(scene.setParent(a, b, a).ok());
    CHECK(names(scene, scene.entities()) == Names{"B", "A2", "A1", "A1x", "A"});

    // Destroying a parent destroys its descendants.
    scene.destroyEntity(a1);
    CHECK_FALSE(scene.isAlive(a1x));
    CHECK(names(scene, scene.entities()) == Names{"B", "A2", "A"});
    CHECK(scene.findByName("A1x").isNull());
}

TEST_CASE("Scene::setParents restores hierarchy order and rejects loops") {
    Scene scene;
    const Entity child = scene.createEntity("Child");
    const Entity other = scene.createEntity("Other");
    const Entity parent = scene.createEntity("Parent");
    const std::pair<Entity, Entity> links[] = {{child, parent}};
    REQUIRE(scene.setParents(links).ok());
    CHECK(names(scene, scene.entities()) == Names{"Other", "Parent", "Child"});
    CHECK(scene.parent(other).isNull());

    const std::pair<Entity, Entity> loop[] = {{parent, child}};
    const Status status = scene.setParents(loop);
    REQUIRE_FALSE(status.ok());
    CHECK(status.error().message.find("loop") != std::string::npos);
    CHECK(scene.parent(parent).isNull()); // unchanged
    CHECK(names(scene, scene.entities()) == Names{"Other", "Parent", "Child"});

    // Depth limit.
    Scene deep;
    Entity last = deep.createEntity("0");
    for (int i = 1; i <= kMaxHierarchyDepth; ++i) last = deep.createEntity(std::to_string(i), last);
    CHECK(deep.depth(last) == kMaxHierarchyDepth);
    CHECK(deep.createEntity("too deep", last).isNull());
}

TEST_CASE("World transforms compose through parents") {
    Scene scene;
    const Entity parent = scene.createEntity("Parent");
    scene.set(parent, Transform{{100, 50}, 0.0f, {2, 2}});
    const Entity child = scene.createEntity("Child", parent);
    scene.set(child, Transform{{10, 5}, 0.0f, {1, 1}});
    CHECK(scene.worldPosition(child) == Vec2{120, 60});
    CHECK(scene.worldTransform(child).scale == Vec2{2, 2});

    // A parent without Transform contributes nothing.
    const Entity group = scene.createEntity("Group");
    const Entity inGroup = scene.createEntity("InGroup", group);
    scene.set(inGroup, Transform{{7, 8}});
    CHECK(scene.worldPosition(inGroup) == Vec2{7, 8});

    // Setting the world position changes the local one.
    scene.setWorldPosition(child, {200, 100});
    CHECK(scene.get<Transform>(child).position == Vec2{50, 25});
    CHECK(scene.worldPosition(child) == Vec2{200, 100});

    // Rotation: a child 10 units right of a parent rotated 90 degrees
    // clockwise (Y-down) ends up 10 units below it.
    scene.get<Transform>(parent).rotation = 90.0f;
    scene.get<Transform>(parent).scale = {1, 1};
    scene.get<Transform>(child).position = {10, 0};
    const Vec2 rotated = scene.worldPosition(child);
    CHECK(rotated.x == doctest::Approx(100.0f));
    CHECK(rotated.y == doctest::Approx(60.0f));
    CHECK(scene.worldTransform(child).rotation == doctest::Approx(90.0f));

    // Re-parenting can keep the world transform.
    REQUIRE(scene.setParent(child, kNullEntity, kNullEntity, true).ok());
    CHECK(scene.get<Transform>(child).position.x == doctest::Approx(100.0f));
    CHECK(scene.get<Transform>(child).position.y == doctest::Approx(60.0f));
    CHECK(scene.get<Transform>(child).rotation == doctest::Approx(90.0f));
    REQUIRE(scene.setParent(child, parent, kNullEntity, true).ok());
    CHECK(scene.worldPosition(child).x == doctest::Approx(100.0f));
    CHECK(scene.worldPosition(child).y == doctest::Approx(60.0f));
    CHECK(scene.get<Transform>(child).rotation == doctest::Approx(0.0f));
}

TEST_CASE("Entities and subtrees can be copied within and between scenes") {
    Scene source;
    const Entity root = source.createEntity("Root");
    source.set(root, Transform{{1, 2}});
    source.set(root, Health{7});
    const Entity kid = source.createEntity("Kid", root);
    source.set(kid, Tag{});

    Scene target;
    const Entity copy = target.copySubtree(source, root);
    REQUIRE_FALSE(copy.isNull());
    CHECK(target.entityCount() == 2);
    CHECK(target.get<Health>(copy).value == 7);
    CHECK(target.get<Transform>(copy).position == Vec2{1, 2});
    CHECK(target.get<EntityInfo>(copy).name == "Root");
    CHECK(target.get<EntityInfo>(copy).id != source.get<EntityInfo>(root).id);
    REQUIRE(target.children(copy).size() == 1);
    CHECK(target.tryGet<Tag>(target.children(copy)[0]) != nullptr);

    // Duplicate within the same scene, inserted before the original.
    const Entity dup = source.copySubtree(source, root, kNullEntity, root);
    REQUIRE_FALSE(dup.isNull());
    CHECK(source.entityCount() == 4);
    CHECK(source.entities()[0] == dup);
    CHECK(source.findById(source.get<EntityInfo>(dup).id) == dup);
    CHECK(source.findById(source.get<EntityInfo>(root).id) == root);
    source.get<Health>(dup).value = 1;
    CHECK(source.get<Health>(root).value == 7);
}
