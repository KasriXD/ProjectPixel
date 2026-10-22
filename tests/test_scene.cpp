#include <doctest.h>

#include "px/scene/Scene.hpp"

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
    CHECK(reg.tryGet<Health>(e)->value == 50);
    reg.set(e, Health{75}); // replace
    CHECK(reg.tryGet<Health>(e)->value == 75);
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
    b.tryGet<Health>(e)->value = 2;
    CHECK(a.tryGet<Health>(e)->value == 1);
    CHECK(b.tryGet<Health>(e)->value == 2);
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
