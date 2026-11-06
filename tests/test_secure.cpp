// Encrypted game traffic (engine/net Secure.hpp, docs/adr/0030).
#include <doctest.h>

#include "px/crypto/Crypto.hpp"
#include "px/net/Loopback.hpp"
#include "px/net/Protocol.hpp"
#include "px/net/Secure.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <string>
#include <string_view>

using namespace px;
using namespace px::net;

namespace {

std::vector<std::uint8_t> bytes(std::string_view s) { return {s.begin(), s.end()}; }

bool contains(const std::vector<std::uint8_t>& haystack, std::string_view needle) {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

// Sits between a transport and the secure layer: records what goes on the
// wire, and can hold, change or inject what arrives.
class Spy final : public INetEndpoint {
public:
    explicit Spy(std::unique_ptr<INetEndpoint> inner) : m_inner(std::move(inner)) {}
    std::vector<std::vector<std::uint8_t>> wire;          // everything sent, as sent
    std::function<bool(NetEvent&)> intercept;             // false: hold the event (in `held`)
    std::deque<NetEvent> held;
    std::deque<NetEvent> injected;                        // delivered on the next poll

    void poll(std::vector<NetEvent>& out) override {
        std::vector<NetEvent> raw;
        m_inner->poll(raw);
        for (auto& e : injected) out.push_back(e);
        injected.clear();
        for (auto& e : raw) {
            if (intercept && !intercept(e)) {
                held.push_back(e);
                continue;
            }
            out.push_back(std::move(e));
        }
    }
    bool send(PeerId peer, Delivery delivery, std::span<const std::uint8_t> data) override {
        wire.emplace_back(data.begin(), data.end());
        return m_inner->send(peer, delivery, data);
    }
    void flush() override { m_inner->flush(); }
    void disconnect(PeerId peer, std::uint32_t reason) override { m_inner->disconnect(peer, reason); }
    [[nodiscard]] PeerStats stats(PeerId peer) const override { return m_inner->stats(peer); }
    [[nodiscard]] std::string describe(PeerId peer) const override { return m_inner->describe(peer); }

private:
    std::unique_ptr<INetEndpoint> m_inner;
};

struct Pair {
    LoopbackNetwork net{7};
    Spy* serverSpy = nullptr;
    Spy* clientSpy = nullptr;
    std::unique_ptr<INetEndpoint> server;
    std::unique_ptr<INetEndpoint> client;
    std::vector<NetEvent> serverEvents;
    std::vector<NetEvent> clientEvents;
    double time = 0.0;

    Pair(SecureServerOptions serverOptions, SecureClientOptions clientOptions) {
        auto listening = net.listen(9000);
        REQUIRE(listening);
        auto s = std::make_unique<Spy>(std::move(listening.value()));
        serverSpy = s.get();
        server = secureServer(std::move(s), std::move(serverOptions));
        auto c = std::make_unique<Spy>(net.connect(9000));
        clientSpy = c.get();
        client = secureClient(std::move(c), clientOptions);
    }

    void step(int times = 1) {
        for (int i = 0; i < times; ++i) {
            time += 0.01;
            net.setTime(time);
            server->poll(serverEvents);
            client->poll(clientEvents);
            server->flush();
            client->flush();
        }
    }

    [[nodiscard]] PeerId connectedPeer(const std::vector<NetEvent>& events) const {
        for (const auto& e : events) {
            if (e.type == NetEvent::Type::Connected) return e.peer;
        }
        return kNoPeer;
    }

    [[nodiscard]] std::vector<std::string> received(std::vector<NetEvent>& events) const {
        std::vector<std::string> out;
        for (const auto& e : events) {
            if (e.type == NetEvent::Type::Received) out.emplace_back(e.data.begin(), e.data.end());
        }
        events.clear();
        return out;
    }

    [[nodiscard]] std::optional<std::uint32_t> disconnectReason(const std::vector<NetEvent>& events) const {
        for (const auto& e : events) {
            if (e.type == NetEvent::Type::Disconnected) return e.reason;
        }
        return std::nullopt;
    }
};

constexpr std::uint32_t code(DisconnectReason r) { return static_cast<std::uint32_t>(r); }

} // namespace

TEST_CASE("Crypto: key exchange and authenticated encryption") {
    auto a = crypto::KeyExchangeKey::generate();
    auto b = crypto::KeyExchangeKey::generate();
    REQUIRE(a);
    REQUIRE(b);
    const auto ab = crypto::keyExchange(*a, b->publicKey);
    const auto ba = crypto::keyExchange(*b, a->publicKey);
    REQUIRE(ab);
    REQUIRE(ba);
    CHECK(*ab == *ba);
    CHECK(crypto::KeyExchangeKey::fromSecret(a->secret).publicKey == a->publicKey);
    // A low-order point (all zero) gives an all-zero secret: refused.
    CHECK_FALSE(crypto::keyExchange(*a, crypto::PublicKey{}).has_value());

    crypto::SecretKey key{};
    key[0] = 1;
    crypto::Nonce nonce{};
    const auto plain = bytes("attack at dawn");
    const auto extra = bytes("header");
    std::vector<std::uint8_t> cipher(plain.size());
    crypto::Mac mac{};
    crypto::seal(cipher, mac, key, nonce, extra, plain);
    CHECK(cipher != plain);
    std::vector<std::uint8_t> back(plain.size());
    CHECK(crypto::open(back, mac, key, nonce, extra, cipher));
    CHECK(back == plain);
    auto changed = cipher;
    changed[3] ^= 1;
    CHECK_FALSE(crypto::open(back, mac, key, nonce, extra, changed));
    CHECK_FALSE(crypto::open(back, mac, key, nonce, bytes("other"), cipher));
    nonce[0] = 1;
    CHECK_FALSE(crypto::open(back, mac, key, nonce, extra, cipher));

    // Keyed hashing depends on the key.
    CHECK(crypto::keyedHash256(bytes("k1"), bytes("x")) != crypto::keyedHash256(bytes("k2"), bytes("x")));

    // Keys as text.
    const auto text = encodeServerKey(a->publicKey);
    CHECK(text.size() == 43);
    auto decoded = decodeServerKey(text);
    REQUIRE(decoded);
    CHECK(*decoded == a->publicKey);
    CHECK_FALSE(decodeServerKey("short").ok());
    auto identity = decodeIdentity(encodeIdentity(*a));
    REQUIRE(identity);
    CHECK(identity->publicKey == a->publicKey);
}

TEST_CASE("Secure: messages are encrypted both ways, and nothing readable is on the wire") {
    auto identity = crypto::KeyExchangeKey::generate();
    REQUIRE(identity);
    Pair p({*identity, 0}, {identity->publicKey, 0});
    p.step(5);
    const PeerId server = p.connectedPeer(p.clientEvents);
    const PeerId client = p.connectedPeer(p.serverEvents);
    REQUIRE(server != kNoPeer);
    REQUIRE(client != kNoPeer);
    p.clientEvents.clear();
    p.serverEvents.clear();

    CHECK(p.client->send(server, Delivery::Reliable, bytes("my name is Mallory's secret")));
    CHECK(p.client->send(server, Delivery::Unreliable, bytes("input frame 1")));
    CHECK(p.server->send(client, Delivery::Unreliable, bytes("snapshot with secret coins")));
    p.step(3);
    const auto atServer = p.received(p.serverEvents);
    const auto atClient = p.received(p.clientEvents);
    CHECK(atServer == std::vector<std::string>{"my name is Mallory's secret", "input frame 1"});
    CHECK(atClient == std::vector<std::string>{"snapshot with secret coins"});
    for (const auto& w : p.clientSpy->wire) CHECK_FALSE(contains(w, "secret"));
    for (const auto& w : p.serverSpy->wire) CHECK_FALSE(contains(w, "secret"));
    // Each message grows by the header and the MAC only.
    CHECK(p.clientSpy->wire.back().size() == std::string_view("input frame 1").size() + kSecureOverheadBytes);
}

TEST_CASE("Secure: replayed, changed and reordered messages") {
    Pair p({}, {});
    p.step(5);
    const PeerId server = p.connectedPeer(p.clientEvents);
    REQUIRE(server != kNoPeer);
    p.serverEvents.clear();

    SUBCASE("a replayed message is dropped") {
        CHECK(p.client->send(server, Delivery::Unreliable, bytes("jump")));
        p.step(2);
        PeerId fromClient = kNoPeer;
        for (const auto& e : p.serverEvents) fromClient = e.peer;
        REQUIRE(fromClient != kNoPeer);
        CHECK(p.received(p.serverEvents) == std::vector<std::string>{"jump"});
        // Someone on the way sends the same bytes again, twice.
        NetEvent copy;
        copy.type = NetEvent::Type::Received;
        copy.peer = fromClient;
        copy.data = p.clientSpy->wire.back();
        p.serverSpy->injected = {copy, copy};
        p.step(2);
        CHECK(p.received(p.serverEvents).empty());
    }

    SUBCASE("unreliable messages may arrive out of order, each once") {
        p.serverSpy->intercept = [](NetEvent& e) { return e.type != NetEvent::Type::Received; };
        for (const char* m : {"a", "b", "c"}) CHECK(p.client->send(server, Delivery::Unreliable, bytes(m)));
        p.step(3);
        REQUIRE(p.serverSpy->held.size() == 3);
        p.serverSpy->intercept = nullptr;
        // Deliver c, a, b, then a again.
        p.serverSpy->injected = {p.serverSpy->held[2], p.serverSpy->held[0], p.serverSpy->held[1], p.serverSpy->held[0]};
        p.step(1);
        CHECK(p.received(p.serverEvents) == std::vector<std::string>{"c", "a", "b"});
    }

    SUBCASE("changed messages are dropped, and many of them end the connection") {
        p.serverSpy->intercept = [](NetEvent& e) {
            if (e.type == NetEvent::Type::Received) e.data.back() ^= 0x40;
            return true;
        };
        CHECK(p.client->send(server, Delivery::Unreliable, bytes("hello")));
        p.step(2);
        CHECK(p.received(p.serverEvents).empty());
        CHECK_FALSE(p.disconnectReason(p.serverEvents).has_value());
        for (int i = 0; i < 20; ++i) CHECK(p.client->send(server, Delivery::Unreliable, bytes("again")));
        p.step(3);
        CHECK(p.disconnectReason(p.serverEvents) == code(DisconnectReason::BadMessage));
    }
}

TEST_CASE("Secure: the client checks the server's identity") {
    auto real = crypto::KeyExchangeKey::generate();
    auto other = crypto::KeyExchangeKey::generate();
    REQUIRE(real);
    REQUIRE(other);

    SUBCASE("the expected key") {
        Pair p({*real, 0}, {real->publicKey, 0});
        p.step(5);
        CHECK(p.connectedPeer(p.clientEvents) != kNoPeer);
    }
    SUBCASE("a different server") {
        Pair p({*other, 0}, {real->publicKey, 0});
        p.step(5);
        CHECK(p.connectedPeer(p.clientEvents) == kNoPeer);
        CHECK(p.disconnectReason(p.clientEvents) == code(DisconnectReason::ServerUnverified));
    }
    SUBCASE("a server without an identity, when one is expected") {
        Pair p({}, {real->publicKey, 0});
        p.step(5);
        CHECK(p.disconnectReason(p.clientEvents) == code(DisconnectReason::ServerUnverified));
    }
    SUBCASE("someone in between who passes on the real identity but answers with their own fresh key") {
        // The server hello is rewritten: the identity stays, the fresh key is
        // the attacker's. The client accepts the hello, but the keys differ, so
        // the attacker can neither read the client nor be read as the server.
        auto attacker = crypto::KeyExchangeKey::generate();
        REQUIRE(attacker);
        Pair p({*real, 0}, {real->publicKey, 0});
        p.clientSpy->intercept = [&](NetEvent& e) {
            if (e.type == NetEvent::Type::Received && e.data.size() == 71 && e.data[0] == 0xC2) {
                std::copy(attacker->publicKey.begin(), attacker->publicKey.end(), e.data.begin() + 6);
            }
            return true;
        };
        p.step(5);
        const PeerId server = p.connectedPeer(p.clientEvents);
        REQUIRE(server != kNoPeer);
        p.serverEvents.clear();
        CHECK(p.client->send(server, Delivery::Reliable, bytes("Hello")));
        p.step(3);
        CHECK(p.received(p.serverEvents).empty()); // the real server cannot open it either
    }
}

TEST_CASE("Secure: players and servers from before encryption get a clear answer") {
    SUBCASE("an older player sends a plain Hello") {
        LoopbackNetwork net(3);
        auto listening = net.listen(9100);
        REQUIRE(listening);
        auto server = secureServer(std::move(listening.value()), {});
        auto old = net.connect(9100);
        std::vector<NetEvent> serverEvents;
        std::vector<NetEvent> oldEvents;
        PeerId peer = kNoPeer;
        double t = 0;
        for (int i = 0; i < 20; ++i) {
            t += 0.01;
            net.setTime(t);
            server->poll(serverEvents);
            old->poll(oldEvents);
            for (const auto& e : oldEvents) {
                if (e.type == NetEvent::Type::Connected && peer == kNoPeer) {
                    peer = e.peer;
                    HelloMessage hello;
                    hello.playerName = "Old";
                    old->send(peer, Delivery::Reliable, encode(hello));
                }
            }
            server->flush();
            old->flush();
        }
        bool rejected = false;
        bool closed = false;
        for (const auto& e : oldEvents) {
            if (e.type == NetEvent::Type::Received && messageType(e.data) == MessageType::Reject) {
                auto reject = decodeReject(e.data);
                REQUIRE(reject);
                rejected = reject->reason == DisconnectReason::VersionMismatch &&
                           reject->text.find("encrypts") != std::string::npos;
            }
            if (e.type == NetEvent::Type::Disconnected) closed = e.reason == code(DisconnectReason::VersionMismatch);
        }
        CHECK(rejected);
        CHECK(closed);
    }
    SUBCASE("an older server calls the handshake a bad message") {
        LoopbackNetwork net(4);
        auto listening = net.listen(9200);
        REQUIRE(listening);
        auto oldServer = std::move(listening.value());
        auto client = secureClient(net.connect(9200), {});
        std::vector<NetEvent> serverEvents;
        std::vector<NetEvent> clientEvents;
        double t = 0;
        for (int i = 0; i < 20; ++i) {
            t += 0.01;
            net.setTime(t);
            serverEvents.clear();
            oldServer->poll(serverEvents);
            for (const auto& e : serverEvents) {
                if (e.type == NetEvent::Type::Received) oldServer->disconnect(e.peer, code(DisconnectReason::BadMessage));
            }
            client->poll(clientEvents);
            oldServer->flush();
            client->flush();
        }
        std::optional<std::uint32_t> reason;
        for (const auto& e : clientEvents) {
            if (e.type == NetEvent::Type::Disconnected) reason = e.reason;
        }
        CHECK(reason == code(DisconnectReason::VersionMismatch));
        CHECK(describeDisconnect(*reason).find("different version") != std::string::npos);
    }
    SUBCASE("garbage instead of a handshake ends the connection") {
        LoopbackNetwork net(5);
        auto listening = net.listen(9300);
        REQUIRE(listening);
        auto server = secureServer(std::move(listening.value()), {});
        auto raw = net.connect(9300);
        std::vector<NetEvent> serverEvents;
        std::vector<NetEvent> rawEvents;
        double t = 0;
        bool sent = false;
        for (int i = 0; i < 20; ++i) {
            t += 0.01;
            net.setTime(t);
            server->poll(serverEvents);
            raw->poll(rawEvents);
            for (const auto& e : rawEvents) {
                if (e.type == NetEvent::Type::Connected && !sent) {
                    sent = raw->send(e.peer, Delivery::Reliable, bytes("\xC1junk"));
                }
            }
            server->flush();
            raw->flush();
        }
        std::optional<std::uint32_t> atServer;
        for (const auto& e : serverEvents) {
            if (e.type == NetEvent::Type::Disconnected) atServer = e.reason;
        }
        CHECK(atServer == code(DisconnectReason::BadMessage));
    }
}
