#include "px/net/Secure.hpp"

#include "px/core/Log.hpp"
#include "px/crypto/Encoding.hpp"
#include "px/net/Protocol.hpp"

#include <array>
#include <cstring>
#include <unordered_map>

namespace px::net {

namespace {

constexpr std::uint8_t kClientHello = 0xC1;
constexpr std::uint8_t kServerHello = 0xC2;
constexpr std::uint8_t kReliableData = 0xD0;
constexpr std::uint8_t kUnreliableData = 0xD1;
constexpr std::array<std::uint8_t, 4> kMagic = {'P', 'X', 'S', 'C'};
constexpr std::size_t kClientHelloBytes = 1 + 4 + 1 + 32;          // kind, magic, version, fresh key
constexpr std::size_t kServerHelloBytes = 1 + 4 + 1 + 32 + 1 + 32; // ... fresh key, has identity, identity
constexpr std::uint64_t kWindow = 128;                              // unreliable messages accepted behind the newest
constexpr int kMaxFailures = 16;                                    // undecryptable messages before giving up

struct Keys {
    crypto::SecretKey send{};
    crypto::SecretKey receive{};
    void wipe() {
        crypto::wipe(send);
        crypto::wipe(receive);
    }
};

// Counters of one connection: separate for reliable and unreliable messages
// (their counters are in each nonce, so they never collide).
struct Counters {
    std::uint64_t nextReliable = 1;
    std::uint64_t nextUnreliable = 1;
    std::uint64_t lastReliable = 0;   // reliable messages arrive in order: each must be newer
    std::uint64_t topUnreliable = 0;  // the newest unreliable counter seen
    std::array<std::uint64_t, 2> seen{}; // bit i: topUnreliable - i was seen (i < 128)

    [[nodiscard]] bool fresh(bool reliable, std::uint64_t counter) const {
        if (counter == 0) return false;
        if (reliable) return counter > lastReliable;
        if (counter > topUnreliable) return true;
        const std::uint64_t behind = topUnreliable - counter;
        if (behind >= kWindow) return false;
        return (seen[behind / 64] & (std::uint64_t{1} << (behind % 64))) == 0;
    }

    void accept(bool reliable, std::uint64_t counter) {
        if (reliable) {
            lastReliable = counter;
            return;
        }
        if (counter > topUnreliable) {
            const std::uint64_t shift = counter - topUnreliable;
            if (shift >= kWindow) {
                seen = {};
            } else {
                // Shift the 128-bit window left by `shift`.
                for (std::uint64_t s = shift; s > 0;) {
                    const std::uint64_t step = std::min<std::uint64_t>(s, 63);
                    seen[1] = (seen[1] << step) | (seen[0] >> (64 - step));
                    seen[0] <<= step;
                    s -= step;
                }
            }
            topUnreliable = counter;
            seen[0] |= 1;
            return;
        }
        const std::uint64_t behind = topUnreliable - counter;
        seen[behind / 64] |= std::uint64_t{1} << (behind % 64);
    }
};

crypto::Nonce nonceFor(std::uint8_t kind, std::uint64_t counter) {
    crypto::Nonce nonce{};
    for (int i = 0; i < 8; ++i) nonce[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(counter >> (8 * i));
    nonce[8] = kind;
    return nonce;
}

std::vector<std::uint8_t> sealMessage(const Keys& keys, Counters& counters, Delivery delivery,
                                      std::span<const std::uint8_t> plain) {
    const bool reliable = delivery == Delivery::Reliable;
    const std::uint8_t kind = reliable ? kReliableData : kUnreliableData;
    const std::uint64_t counter = reliable ? counters.nextReliable++ : counters.nextUnreliable++;
    std::vector<std::uint8_t> out(kSecureOverheadBytes + plain.size());
    out[0] = kind;
    for (int i = 0; i < 8; ++i) out[1 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(counter >> (8 * i));
    crypto::Mac mac{};
    const std::array<std::uint8_t, 1> extra{kind};
    crypto::seal(std::span(out).subspan(kSecureOverheadBytes), mac, keys.send, nonceFor(kind, counter), extra, plain);
    std::memcpy(out.data() + 9, mac.data(), mac.size());
    return out;
}

// Decrypts a data message; nullopt if it is not one, was changed, or was seen before.
std::optional<std::vector<std::uint8_t>> openMessage(const Keys& keys, Counters& counters,
                                                     std::span<const std::uint8_t> data) {
    if (data.size() < kSecureOverheadBytes) return std::nullopt;
    const std::uint8_t kind = data[0];
    if (kind != kReliableData && kind != kUnreliableData) return std::nullopt;
    const bool reliable = kind == kReliableData;
    std::uint64_t counter = 0;
    for (int i = 0; i < 8; ++i) counter |= std::uint64_t{data[1 + static_cast<std::size_t>(i)]} << (8 * i);
    if (!counters.fresh(reliable, counter)) return std::nullopt;
    crypto::Mac mac{};
    std::memcpy(mac.data(), data.data() + 9, mac.size());
    std::vector<std::uint8_t> plain(data.size() - kSecureOverheadBytes);
    const std::array<std::uint8_t, 1> extra{kind};
    if (!crypto::open(plain, mac, keys.receive, nonceFor(kind, counter), extra, data.subspan(kSecureOverheadBytes))) {
        return std::nullopt;
    }
    counters.accept(reliable, counter);
    return plain;
}

// Both sides: one key per direction from the two exchanges and everything
// the handshake said.
Keys deriveKeys(bool client, const crypto::PublicKey& clientFresh, const crypto::PublicKey& serverFresh,
                const crypto::PublicKey* identity, std::array<std::uint8_t, 32>& freshShared,
                std::array<std::uint8_t, 32>& identityShared) {
    crypto::Hasher h;
    h.update(crypto::bytesOf("Project Pixel game traffic 1"));
    h.update(clientFresh);
    h.update(serverFresh);
    const crypto::PublicKey none{};
    h.update(identity != nullptr ? *identity : none);
    h.update(freshShared);
    h.update(identityShared);
    crypto::Hash256 chain = h.finish();
    const crypto::Hash256 toServer = crypto::keyedHash256(chain, crypto::bytesOf("client to server"));
    const crypto::Hash256 toClient = crypto::keyedHash256(chain, crypto::bytesOf("server to client"));
    Keys keys;
    keys.send = client ? toServer : toClient;
    keys.receive = client ? toClient : toServer;
    crypto::wipe(chain);
    crypto::wipe(freshShared);
    crypto::wipe(identityShared);
    return keys;
}

bool hasMagic(std::span<const std::uint8_t> data) {
    return data.size() >= 6 && std::equal(kMagic.begin(), kMagic.end(), data.begin() + 1) && data[5] == kSecureVersion;
}

NetEvent disconnected(PeerId peer, std::uint32_t reason) {
    NetEvent e;
    e.type = NetEvent::Type::Disconnected;
    e.peer = peer;
    e.reason = reason;
    return e;
}

constexpr std::uint32_t code(DisconnectReason r) { return static_cast<std::uint32_t>(r); }

// --- Server ------------------------------------------------------------------------------------

class SecureServer final : public INetEndpoint {
public:
    SecureServer(std::unique_ptr<INetEndpoint> inner, SecureServerOptions options)
        : m_inner(std::move(inner)), m_options(std::move(options)) {}

    ~SecureServer() override {
        for (auto& [id, peer] : m_peers) peer.keys.wipe();
        if (m_options.identity) crypto::wipe(m_options.identity->secret);
    }

    // Connections are reported at once (the game server's Hello timeout,
    // in game time, also covers the handshake); messages only once the
    // handshake is done.
    void poll(std::vector<NetEvent>& out) override {
        m_raw.clear();
        m_inner->poll(m_raw);
        for (NetEvent& e : m_raw) {
            switch (e.type) {
                case NetEvent::Type::Connected:
                    m_peers[e.peer] = Peer{};
                    out.push_back(std::move(e));
                    break;
                case NetEvent::Type::Disconnected: {
                    const auto it = m_peers.find(e.peer);
                    if (it == m_peers.end()) break;
                    it->second.keys.wipe();
                    m_peers.erase(it);
                    out.push_back(std::move(e));
                    break;
                }
                case NetEvent::Type::Received: received(e, out); break;
            }
        }
    }

    bool send(PeerId peer, Delivery delivery, std::span<const std::uint8_t> data) override {
        const auto it = m_peers.find(peer);
        if (it == m_peers.end() || !it->second.ready) return false;
        return m_inner->send(peer, delivery, sealMessage(it->second.keys, it->second.counters, delivery, data));
    }

    void flush() override { m_inner->flush(); }

    void disconnect(PeerId peer, std::uint32_t reason) override {
        m_inner->disconnect(peer, reason);
        if (const auto it = m_peers.find(peer); it != m_peers.end()) {
            it->second.keys.wipe();
            m_peers.erase(it);
        }
    }

    [[nodiscard]] PeerStats stats(PeerId peer) const override { return m_inner->stats(peer); }
    [[nodiscard]] std::string describe(PeerId peer) const override { return m_inner->describe(peer); }

private:
    struct Peer {
        bool ready = false;
        int failures = 0;
        Keys keys;
        Counters counters;
    };

    void received(NetEvent& e, std::vector<NetEvent>& out) {
        const auto it = m_peers.find(e.peer);
        if (it == m_peers.end()) return;
        Peer& peer = it->second;
        if (peer.ready) {
            const std::size_t limit = m_options.maxMessageBytes;
            auto plain = limit > 0 && e.data.size() > limit + kSecureOverheadBytes
                             ? std::nullopt
                             : openMessage(peer.keys, peer.counters, e.data);
            if (!plain) {
                if (++peer.failures > kMaxFailures) {
                    log::warn("Secure: {} sent {} messages that could not be decrypted", m_inner->describe(e.peer),
                              peer.failures);
                    const PeerId id = e.peer;
                    disconnect(id, code(DisconnectReason::BadMessage));
                    out.push_back(disconnected(id, code(DisconnectReason::BadMessage)));
                }
                return;
            }
            e.data = std::move(*plain);
            out.push_back(std::move(e));
            return;
        }
        if (e.data.size() == kClientHelloBytes && e.data[0] == kClientHello && hasMagic(e.data)) {
            handshake(e.peer, peer, e.data, out);
            return;
        }
        const bool olderPlayer = !e.data.empty() && e.data[0] == static_cast<std::uint8_t>(MessageType::Hello);
        if (olderPlayer) {
            // A player from before encryption: tell it why, in words it understands.
            RejectMessage reject;
            reject.reason = DisconnectReason::VersionMismatch;
            reject.text = "This server encrypts game traffic: update Project Pixel to play here";
            m_inner->send(e.peer, Delivery::Reliable, encode(reject));
            m_inner->flush();
        }
        const std::uint32_t reason = code(olderPlayer ? DisconnectReason::VersionMismatch : DisconnectReason::BadMessage);
        const PeerId id = e.peer;
        disconnect(id, reason);
        out.push_back(disconnected(id, reason));
    }

    void handshake(PeerId id, Peer& peer, std::span<const std::uint8_t> hello, std::vector<NetEvent>& out) {
        crypto::PublicKey clientFresh{};
        std::memcpy(clientFresh.data(), hello.data() + 6, clientFresh.size());
        auto fresh = crypto::KeyExchangeKey::generate();
        if (!fresh) {
            disconnect(id, code(DisconnectReason::ShuttingDown));
            out.push_back(disconnected(id, code(DisconnectReason::ShuttingDown)));
            return;
        }
        auto freshShared = crypto::keyExchange(*fresh, clientFresh);
        std::optional<std::array<std::uint8_t, 32>> identityShared = std::array<std::uint8_t, 32>{};
        if (m_options.identity) identityShared = crypto::keyExchange(*m_options.identity, clientFresh);
        if (!freshShared || !identityShared) {
            crypto::wipe(fresh->secret);
            disconnect(id, code(DisconnectReason::BadMessage));
            out.push_back(disconnected(id, code(DisconnectReason::BadMessage)));
            return;
        }
        const crypto::PublicKey* identity = m_options.identity ? &m_options.identity->publicKey : nullptr;
        peer.keys = deriveKeys(false, clientFresh, fresh->publicKey, identity, *freshShared, *identityShared);
        std::vector<std::uint8_t> reply(kServerHelloBytes, 0);
        reply[0] = kServerHello;
        std::copy(kMagic.begin(), kMagic.end(), reply.begin() + 1);
        reply[5] = kSecureVersion;
        std::copy(fresh->publicKey.begin(), fresh->publicKey.end(), reply.begin() + 6);
        reply[38] = identity != nullptr ? 1 : 0;
        if (identity != nullptr) std::copy(identity->begin(), identity->end(), reply.begin() + 39);
        crypto::wipe(fresh->secret);
        m_inner->send(id, Delivery::Reliable, reply);
        peer.ready = true;
    }

    std::unique_ptr<INetEndpoint> m_inner;
    SecureServerOptions m_options;
    std::unordered_map<PeerId, Peer> m_peers;
    std::vector<NetEvent> m_raw;
};

// --- Client ------------------------------------------------------------------------------------

class SecureClient final : public INetEndpoint {
public:
    SecureClient(std::unique_ptr<INetEndpoint> inner, SecureClientOptions options)
        : m_inner(std::move(inner)), m_options(options) {}

    ~SecureClient() override {
        m_keys.wipe();
        crypto::wipe(m_fresh.secret);
    }

    void poll(std::vector<NetEvent>& out) override {
        m_raw.clear();
        m_inner->poll(m_raw);
        for (NetEvent& e : m_raw) {
            if (m_state == State::Closed) break;
            switch (e.type) {
                case NetEvent::Type::Connected: start(e.peer, out); break;
                case NetEvent::Type::Disconnected: {
                    // A server from before encryption does not understand the
                    // handshake and calls it a bad message.
                    if (m_state != State::Ready && e.reason == code(DisconnectReason::BadMessage)) {
                        e.reason = code(DisconnectReason::VersionMismatch);
                    }
                    m_state = State::Closed;
                    out.push_back(std::move(e));
                    break;
                }
                case NetEvent::Type::Received: received(e, out); break;
            }
        }
    }

    bool send(PeerId peer, Delivery delivery, std::span<const std::uint8_t> data) override {
        if (m_state != State::Ready || peer != m_server) return false;
        return m_inner->send(peer, delivery, sealMessage(m_keys, m_counters, delivery, data));
    }

    void flush() override { m_inner->flush(); }

    void disconnect(PeerId peer, std::uint32_t reason) override {
        m_inner->disconnect(peer, reason);
        m_state = State::Closed;
    }

    [[nodiscard]] PeerStats stats(PeerId peer) const override { return m_inner->stats(peer); }
    [[nodiscard]] std::string describe(PeerId peer) const override { return m_inner->describe(peer); }

private:
    enum class State { Connecting, Handshaking, Ready, Closed };

    void start(PeerId server, std::vector<NetEvent>& out) {
        m_server = server;
        auto fresh = crypto::KeyExchangeKey::generate();
        if (!fresh) {
            fail(code(DisconnectReason::None), out);
            return;
        }
        m_fresh = *fresh;
        crypto::wipe(fresh->secret);
        std::vector<std::uint8_t> hello(kClientHelloBytes, 0);
        hello[0] = kClientHello;
        std::copy(kMagic.begin(), kMagic.end(), hello.begin() + 1);
        hello[5] = kSecureVersion;
        std::copy(m_fresh.publicKey.begin(), m_fresh.publicKey.end(), hello.begin() + 6);
        m_inner->send(server, Delivery::Reliable, hello);
        m_inner->flush();
        m_state = State::Handshaking;
    }

    void received(NetEvent& e, std::vector<NetEvent>& out) {
        if (m_state == State::Ready) {
            const std::size_t limit = m_options.maxMessageBytes;
            auto plain = limit > 0 && e.data.size() > limit + kSecureOverheadBytes ? std::nullopt
                                                                                   : openMessage(m_keys, m_counters, e.data);
            if (!plain) {
                if (++m_failures > kMaxFailures) fail(code(DisconnectReason::BadMessage), out);
                return;
            }
            e.data = std::move(*plain);
            out.push_back(std::move(e));
            return;
        }
        if (m_state != State::Handshaking) return;
        const auto& d = e.data;
        if (d.size() != kServerHelloBytes || d[0] != kServerHello || !hasMagic(d) || d[38] > 1) {
            fail(code(DisconnectReason::BadMessage), out);
            return;
        }
        crypto::PublicKey serverFresh{};
        std::memcpy(serverFresh.data(), d.data() + 6, serverFresh.size());
        const bool hasIdentity = d[38] == 1;
        crypto::PublicKey identity{};
        if (hasIdentity) std::memcpy(identity.data(), d.data() + 39, identity.size());
        if (m_options.expectedServer &&
            (!hasIdentity || !crypto::constantTimeEqual(identity, *m_options.expectedServer))) {
            log::warn("Secure: the server's key is not the one expected");
            fail(code(DisconnectReason::ServerUnverified), out);
            return;
        }
        auto freshShared = crypto::keyExchange(m_fresh, serverFresh);
        std::optional<std::array<std::uint8_t, 32>> identityShared = std::array<std::uint8_t, 32>{};
        if (hasIdentity) identityShared = crypto::keyExchange(m_fresh, identity);
        crypto::wipe(m_fresh.secret);
        if (!freshShared || !identityShared) {
            fail(code(DisconnectReason::BadMessage), out);
            return;
        }
        m_keys = deriveKeys(true, m_fresh.publicKey, serverFresh, hasIdentity ? &identity : nullptr, *freshShared,
                            *identityShared);
        m_state = State::Ready;
        NetEvent connected;
        connected.type = NetEvent::Type::Connected;
        connected.peer = m_server;
        out.push_back(std::move(connected));
    }

    void fail(std::uint32_t reason, std::vector<NetEvent>& out) {
        if (m_server != kNoPeer) m_inner->disconnect(m_server, reason);
        m_inner->flush();
        m_state = State::Closed;
        out.push_back(disconnected(m_server, reason));
    }

    std::unique_ptr<INetEndpoint> m_inner;
    SecureClientOptions m_options;
    State m_state = State::Connecting;
    PeerId m_server = kNoPeer;
    crypto::KeyExchangeKey m_fresh;
    Keys m_keys;
    Counters m_counters;
    int m_failures = 0;
    std::vector<NetEvent> m_raw;
};

} // namespace

std::unique_ptr<INetEndpoint> secureServer(std::unique_ptr<INetEndpoint> inner, SecureServerOptions options) {
    return std::make_unique<SecureServer>(std::move(inner), std::move(options));
}

std::unique_ptr<INetEndpoint> secureClient(std::unique_ptr<INetEndpoint> inner, SecureClientOptions options) {
    return std::make_unique<SecureClient>(std::move(inner), options);
}

std::string encodeServerKey(const crypto::PublicKey& key) { return crypto::base64UrlEncode(key); }

Result<crypto::PublicKey> decodeServerKey(std::string_view text) {
    auto bytes = crypto::base64UrlDecode(text);
    if (!bytes || bytes->size() != 32) {
        return makeError(ErrorCode::InvalidArgument, "Not a server key (43 characters of base64url)");
    }
    crypto::PublicKey key{};
    std::copy(bytes->begin(), bytes->end(), key.begin());
    return key;
}

std::string encodeIdentity(const crypto::KeyExchangeKey& key) { return crypto::base64UrlEncode(key.secret); }

Result<crypto::KeyExchangeKey> decodeIdentity(std::string_view text) {
    auto bytes = crypto::base64UrlDecode(text);
    if (!bytes || bytes->size() != 32) return makeError(ErrorCode::InvalidArgument, "Not a server identity key");
    std::array<std::uint8_t, 32> secret{};
    std::copy(bytes->begin(), bytes->end(), secret.begin());
    auto key = crypto::KeyExchangeKey::fromSecret(secret);
    crypto::wipe(*bytes);
    crypto::wipe(secret);
    return key;
}

} // namespace px::net
