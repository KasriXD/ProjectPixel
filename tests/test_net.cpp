// Networking building blocks: byte encoding, the protocol messages, the
// loopback transport and snapshot replication.

#include <doctest.h>

#include "px/net/Bytes.hpp"
#include "px/net/Loopback.hpp"
#include "px/net/Protocol.hpp"
#include "px/net/Replication.hpp"

#include <cmath>
#include <limits>
#include <random>

using namespace px;
using namespace px::net;
using namespace px::scene;

namespace {

std::vector<NetEvent> pollAll(INetEndpoint& endpoint) {
    std::vector<NetEvent> events;
    endpoint.poll(events);
    return events;
}

std::vector<std::uint8_t> bytesOf(std::string_view text) { return {text.begin(), text.end()}; }

// A small level: ground, a player-like body, a coin with tags, a group with a child.
Scene replicationScene() {
    Scene scene;
    const Entity level = scene.createEntity("Level");
    scene.set(level, Transform{});
    Tilemap map;
    map.width = 4;
    map.height = 2;
    map.tiles = {0, 1, -1, 2, 3, 3, 3, 3};
    scene.set(level, map);
    const Entity body = scene.createEntity("Body");
    scene.set(body, Transform{{10, 20}});
    scene.set(body, Sprite{});
    scene.set(body, BoxCollider{{10, 14}, {0, 1}, false});
    scene.set(body, KinematicBody{});
    scene.set(body, PlatformerController{});
    const Entity coin = scene.createEntity("Coin");
    scene.set(coin, Transform{{50, 20}});
    scene.set(coin, Tags{{"coin"}});
    SpriteAnimation anim;
    anim.frames = {{0, 0, 16, 16}, {16, 0, 16, 16}};
    scene.set(coin, anim);
    const Entity group = scene.createEntity("Group");
    const Entity child = scene.createEntity("Child", group);
    scene.set(child, Transform{{1, 2}});
    const Entity cam = scene.createEntity("Camera");
    scene.set(cam, Camera{});
    return scene;
}

void checkSameWorld(const WorldState& a, const WorldState& b) {
    REQUIRE(a.entities.size() == b.entities.size());
    for (const EntityState& ea : a.entities) {
        const EntityState* eb = b.find(ea.id);
        REQUIRE(eb != nullptr);
        CHECK(eb->guid == ea.guid);
        CHECK(eb->name == ea.name);
        CHECK(eb->parent == ea.parent);
        CHECK(eb->components == ea.components);
        CHECK(eb->data == ea.data);
        CHECK(eb->fieldStarts == ea.fieldStarts);
    }
    CHECK(a.game == b.game);
}

WorldState roundTrip(const WorldState& baseline, const WorldState& current) {
    ByteWriter w;
    writeWorldDelta(w, baseline, current);
    ByteReader r(w.data());
    auto decoded = readWorldDelta(r, baseline);
    REQUIRE(decoded);
    return decoded.value();
}

} // namespace

TEST_CASE("Net: bytes round-trip and reject bad input") {
    ByteWriter w;
    w.u8(7);
    w.u16(0xBEEF);
    w.u32(0xDEADBEEF);
    w.u64(0x0123456789ABCDEFULL);
    w.varint(0);
    w.varint(300);
    w.varint(std::numeric_limits<std::uint64_t>::max());
    w.svarint(-1);
    w.svarint(std::numeric_limits<std::int64_t>::min());
    w.f32(-2.5f);
    w.boolean(true);
    w.string("hello");
    w.guid(Guid(1, 2));
    ByteReader r(w.data());
    CHECK(r.u8() == 7);
    CHECK(r.u16() == 0xBEEF);
    CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.u64() == 0x0123456789ABCDEFULL);
    CHECK(r.varint() == 0);
    CHECK(r.varint() == 300);
    CHECK(r.varint() == std::numeric_limits<std::uint64_t>::max());
    CHECK(r.svarint() == -1);
    CHECK(r.svarint() == std::numeric_limits<std::int64_t>::min());
    CHECK(r.f32() == -2.5f);
    CHECK(r.boolean());
    CHECK(r.string(5) == "hello");
    CHECK(r.guid() == Guid(1, 2));
    CHECK(r.ok());
    CHECK(r.atEnd());
    CHECK(r.u8() == 0); // reading past the end fails, for good
    CHECK_FALSE(r.ok());

    SUBCASE("limits") {
        ByteWriter s;
        s.string("too long");
        ByteReader sr(s.data());
        (void)sr.string(3);
        CHECK_FALSE(sr.ok());

        ByteWriter nan;
        nan.f32(std::numeric_limits<float>::quiet_NaN());
        ByteReader nr(nan.data());
        (void)nr.f32();
        CHECK_FALSE(nr.ok());

        const std::vector<std::uint8_t> endless(11, 0xFF); // varint that never ends
        ByteReader vr(endless);
        (void)vr.varint();
        CHECK_FALSE(vr.ok());

        ByteWriter c;
        c.varint(1000);
        ByteReader cr(c.data());
        (void)cr.count(10'000, 1); // 1000 items cannot fit in 0 remaining bytes
        CHECK_FALSE(cr.ok());

        const std::vector<std::uint8_t> two{2};
        ByteReader br(two);
        (void)br.boolean();
        CHECK_FALSE(br.ok());
    }
}

TEST_CASE("Net: protocol messages round-trip; malformed ones are rejected") {
    HelloMessage hello;
    hello.engineVersion = "0.4.0";
    hello.contentHash = 0x1234;
    hello.playerName = "Ada";
    auto h = decodeHello(encode(hello));
    REQUIRE(h);
    CHECK(h->engineVersion == "0.4.0");
    CHECK(h->contentHash == 0x1234);
    CHECK(h->playerName == "Ada");
    CHECK(messageType(encode(hello)) == MessageType::Hello);

    InputMessage input;
    input.ackEpoch = 2;
    input.ackTick = 99;
    input.lastSequence = 10;
    input.frames = {{1, 1}, {3, 2}, {0, 0}};
    auto in = decodeInput(encode(input));
    REQUIRE(in);
    CHECK(in->ackTick == 99);
    CHECK(in->lastSequence == 10);
    REQUIRE(in->frames.size() == 3);
    CHECK(in->frames[1].held == 3);
    CHECK(in->frames[1].pressed == 2);

    WelcomeMessage welcome;
    welcome.player = 3;
    welcome.serverName = "Test";
    auto wm = decodeWelcome(encode(welcome));
    REQUIRE(wm);
    CHECK(wm->player == 3);
    CHECK(wm->serverName == "Test");

    RejectMessage reject{DisconnectReason::ServerFull, "full"};
    auto rm = decodeReject(encode(reject));
    REQUIRE(rm);
    CHECK(rm->reason == DisconnectReason::ServerFull);

    // Wrong type, truncation, trailing bytes, too many frames.
    CHECK_FALSE(decodeHello(encode(input)));
    auto truncated = encode(hello);
    truncated.pop_back();
    CHECK_FALSE(decodeHello(truncated));
    auto trailing = encode(welcome);
    trailing.push_back(0);
    CHECK_FALSE(decodeWelcome(trailing));
    InputMessage flood;
    flood.lastSequence = 100;
    flood.frames.resize(kMaxInputFramesPerMessage + 1);
    CHECK_FALSE(decodeInput(encode(flood)));
    InputMessage beforeStart;
    beforeStart.lastSequence = 1;
    beforeStart.frames.resize(2); // sequence 0 does not exist
    CHECK_FALSE(decodeInput(encode(beforeStart)));
    CHECK_FALSE(messageType({}));
    CHECK_FALSE(messageType(bytesOf("\x63")));

    CHECK(sanitizePlayerName("  Ada \n Lovelace  ") == "Ada  Lovelace");
    CHECK(sanitizePlayerName("\x01\x02") == "");
    CHECK(sanitizePlayerName(std::string(100, 'x')).size() == runtime::kMaxPlayerNameLength);
    CHECK(describeDisconnect(static_cast<std::uint32_t>(DisconnectReason::ServerFull)) == "the server is full");
}

TEST_CASE("Net: decoders survive random and mutated input") {
    std::mt19937 rng(1234);
    HelloMessage hello;
    hello.engineVersion = "0.4.0";
    hello.playerName = "Ada";
    InputMessage input;
    input.lastSequence = 5;
    input.frames = {{1, 0}, {1, 1}};
    const std::vector<std::vector<std::uint8_t>> valid = {encode(hello), encode(input), encode(WelcomeMessage{1, 60, 2, "s"}),
                                                          encode(RejectMessage{DisconnectReason::Kicked, "x"})};
    const Scene scene = replicationScene();
    NetIdMap ids;
    ids.assignInitial(scene);
    const WorldState world = captureWorld(scene, ids, 1);
    ByteWriter full;
    writeWorldDelta(full, WorldState{}, world);

    int decodedOk = 0;
    for (int i = 0; i < 4000; ++i) {
        std::vector<std::uint8_t> data;
        if (i % 2 == 0) {
            data.resize(rng() % 64);
            for (auto& b : data) b = static_cast<std::uint8_t>(rng());
            if (!data.empty()) data[0] = static_cast<std::uint8_t>(1 + rng() % 18);
        } else {
            data = i % 4 == 1 ? valid[rng() % valid.size()] : full.data();
            const int flips = 1 + static_cast<int>(rng() % 4);
            for (int f = 0; f < flips && !data.empty(); ++f) data[rng() % data.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
            if (rng() % 3 == 0 && !data.empty()) data.resize(rng() % data.size());
        }
        decodedOk += decodeHello(data) ? 1 : 0;
        decodedOk += decodeInput(data) ? 1 : 0;
        decodedOk += decodeWelcome(data) ? 1 : 0;
        decodedOk += decodeReject(data) ? 1 : 0;
        ByteReader r(data);
        decodedOk += readWorldDelta(r, WorldState{}) ? 1 : 0;
        ByteReader r2(data);
        decodedOk += readWorldDelta(r2, world) ? 1 : 0;
    }
    MESSAGE("random/mutated messages that still decoded: " << decodedOk);
    CHECK(true); // the point is: no crash, no sanitizer report
}

TEST_CASE("Net: world deltas reproduce the current state from any baseline") {
    Scene scene = replicationScene();
    NetIdMap ids;
    ids.assignInitial(scene);
    const WorldState initial = captureWorld(scene, ids, 0);
    CHECK(initial.entities.size() == scene.entityCount());
    CHECK(initial.find(1) != nullptr);

    // Nothing changed: the delta is tiny.
    ByteWriter same;
    writeWorldDelta(same, initial, initial);
    CHECK(same.size() <= 4);
    checkSameWorld(roundTrip(initial, initial), initial);

    // Changes of every kind.
    const Entity body = scene.findByName("Body");
    scene.get<Transform>(body).position = {12.5f, 19.0f};
    scene.get<KinematicBody>(body).velocity = {30, -4};
    scene.get<Sprite>(body).flipX = true;
    scene.registry().remove<PlatformerController>(body);
    scene.set(body, TopDownController{});
    scene.get<Tags>(scene.findByName("Coin")).values.push_back("shiny");
    scene.get<Tilemap>(scene.findByName("Level")).tiles[2] = 5;
    scene.destroyEntity(scene.findByName("Group")); // with its child
    const Entity fresh = scene.createEntity("Fresh");
    scene.set(fresh, Transform{{5, 5}});
    scene.set(fresh, Player{{5, 5}, 2, "Bo"});
    const Entity freshChild = scene.createEntity("FreshChild", fresh);
    scene.set(freshChild, Sprite{});
    (void)scene.setParent(scene.findByName("Coin"), fresh);
    WorldState changed = captureWorld(scene, ids, 5);
    changed.game.score = 7;
    changed.game.players = {{1, "Ada"}, {2, "Bo"}};
    changed.game.objective = "GO";

    const WorldState fromInitial = roundTrip(initial, changed);
    checkSameWorld(fromInitial, changed);
    const WorldState fromEmpty = roundTrip(WorldState{}, changed);
    checkSameWorld(fromEmpty, changed);

    // And back.
    checkSameWorld(roundTrip(changed, initial), initial);

    // A delta against the wrong baseline is detected (unknown entity).
    ByteWriter w;
    writeWorldDelta(w, changed, initial);
    ByteReader r(w.data());
    CHECK_FALSE(readWorldDelta(r, WorldState{}));
}

TEST_CASE("Net: a client's scene replica follows the world states") {
    const Scene original = replicationScene();
    Scene server = original;
    Scene client = original;
    NetIdMap ids;
    ids.assignInitial(server);
    const WorldState initial = captureWorld(server, ids, 0);

    // The server moves things, removes the group, adds a character.
    scene::Scene& s = server;
    s.get<Transform>(s.findByName("Body")).position = {40, 20};
    s.destroyEntity(s.findByName("Group"));
    const Entity fresh = s.createEntity("Hero");
    s.set(fresh, Transform{{70, 20}});
    s.set(fresh, Player{{70, 20}, 1, "Ada"});
    const Entity hat = s.createEntity("Hat", fresh);
    s.set(hat, Transform{{0, -8}});
    const WorldState state = roundTrip(initial, captureWorld(s, ids, 2));

    SceneReplica replica(client);
    replica.apply(state);
    CHECK(client.findByName("Group").isNull());
    CHECK(client.findByName("Child").isNull());
    const Entity hero = client.findByName("Hero");
    REQUIRE_FALSE(hero.isNull());
    CHECK(client.get<EntityInfo>(hero).id == s.get<EntityInfo>(fresh).id);
    CHECK(client.get<Player>(hero).name == "Ada");
    CHECK(client.worldPosition(client.findByName("Hat")) == Vec2{70, 12});
    CHECK(client.get<Transform>(client.findByName("Body")).position == Vec2{40, 20});
    // Client-only components are left alone.
    CHECK(client.tryGet<Camera>(client.findByName("Camera")) != nullptr);
    CHECK(client.tryGet<SpriteAnimation>(client.findByName("Coin")) != nullptr);

    // The replicated part of the client scene now equals the server's.
    NetIdMap clientIds;
    clientIds.assignInitial(original);
    const WorldState mirrored = captureWorld(client, clientIds, 2);
    for (const EntityState& e : state.entities) {
        const EntityState* m = mirrored.find(clientIds.find(e.guid));
        REQUIRE(m != nullptr);
        CHECK(m->data == e.data);
        CHECK(m->components == e.components);
    }

    // Interpolation: halfway between two positions; a jump is not blended.
    s.get<Transform>(s.findByName("Body")).position = {50, 20};
    s.get<Transform>(fresh).position = {500, 20};
    const WorldState later = roundTrip(state, captureWorld(s, ids, 4));
    replica.apply(state, &later, 0.5f);
    CHECK(client.get<Transform>(client.findByName("Body")).position.x == doctest::Approx(45.0f));
    CHECK(client.get<Transform>(hero).position.x == doctest::Approx(70.0f));
    replica.apply(later);
    CHECK(client.get<Transform>(hero).position.x == doctest::Approx(500.0f));
}

TEST_CASE("Net: loopback transport connects, orders reliable messages, drops and delays unreliable ones") {
    LoopbackNetwork net(42);
    auto server = net.listen(7000, 2);
    REQUIRE(server);
    LinkConditions lag;
    lag.latencyMs = 50;
    lag.jitterMs = 20;
    auto client = net.connect(7000, lag);

    CHECK(pollAll(*client).empty());
    net.setTime(0.049);
    CHECK(pollAll(*server.value()).empty());
    net.setTime(0.050);
    auto events = pollAll(*server.value());
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == NetEvent::Type::Connected);
    const PeerId peer = events[0].peer;
    net.setTime(0.200); // a round trip, give or take the jitter
    events = pollAll(*client);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == NetEvent::Type::Connected);

    // Reliable: all arrive, in order, despite jitter.
    for (int i = 0; i < 50; ++i) {
        const std::uint8_t b = static_cast<std::uint8_t>(i);
        CHECK(client->send(1, Delivery::Reliable, std::span(&b, 1)));
    }
    net.setTime(1.0);
    events = pollAll(*server.value());
    REQUIRE(events.size() == 50);
    for (int i = 0; i < 50; ++i) CHECK(events[static_cast<std::size_t>(i)].data[0] == i);

    // Unreliable with loss: some are lost.
    LinkConditions lossy;
    lossy.lossPercent = 30;
    auto client2 = net.connect(7000, lossy);
    pollAll(*server.value());
    pollAll(*client2);
    for (int i = 0; i < 200; ++i) {
        const std::uint8_t b = 1;
        (void)client2->send(1, Delivery::Unreliable, std::span(&b, 1));
    }
    events = pollAll(*server.value());
    CHECK(events.size() > 100);
    CHECK(events.size() < 180);

    // Full server refuses a third client; nobody on another port refuses too.
    auto client3 = net.connect(7000);
    pollAll(*server.value());
    events = pollAll(*client3);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == NetEvent::Type::Disconnected);
    CHECK(events[0].reason == kReasonRefused);
    auto nowhere = net.connect(9999);
    events = pollAll(*nowhere);
    REQUIRE(events.size() == 1);
    CHECK(events[0].reason == kReasonRefused);

    // Disconnect with a reason reaches the other side; a vanished client is noticed.
    server.value()->disconnect(peer, 5);
    net.setTime(2.0);
    events = pollAll(*client);
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == NetEvent::Type::Disconnected);
    CHECK(events[0].reason == 5);
    client2.reset();
    events = pollAll(*server.value());
    REQUIRE(events.size() == 1);
    CHECK(events[0].type == NetEvent::Type::Disconnected);
}
