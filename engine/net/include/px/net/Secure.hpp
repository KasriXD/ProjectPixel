#pragma once

#include "px/crypto/Crypto.hpp"
#include "px/net/Transport.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>

// Encrypted game traffic (docs/multiplayer.md#encryption, ADR 0030). A
// layer between the transport (UDP, WebSockets, loopback) and the game
// protocol: every message after a short handshake is encrypted and
// authenticated, so nobody on the way can read or change it, and messages
// replayed by someone else are dropped. GameServer and GameClient always
// use it.
//
// Handshake (one round trip, before the game's Hello):
//   client -> ClientHello: a fresh X25519 key
//   server -> ServerHello: a fresh X25519 key and, if it has one, its
//             long-lived identity key
// Both sides hash the two Diffie-Hellman results (fresh/fresh and
// fresh/identity) into one key per direction. A client that knows which
// identity to expect (platform games: the join ticket carries it) refuses
// any other, so a server in between cannot pretend; without one (a direct
// connection) the traffic is still encrypted against anyone listening.
//
// Messages: a kind byte, a 64-bit counter, a 16-byte MAC and the
// encrypted message (XChaCha20-Poly1305; 25 bytes more than the message).
// Reliable and unreliable messages count separately; each counter value is
// accepted once (unreliable ones within a window of 128 behind the newest).
namespace px::net {

inline constexpr std::uint8_t kSecureVersion = 1;
inline constexpr std::size_t kSecureOverheadBytes = 1 + 8 + 16;

struct SecureServerOptions {
    // The server's long-lived key (platform game servers get one from the
    // platform). Without it, clients cannot check who they talk to.
    std::optional<crypto::KeyExchangeKey> identity;
    std::size_t maxMessageBytes = 0; // largest message accepted after decryption (0: no limit)
};

struct SecureClientOptions {
    // The identity the server must prove (from the join ticket). Without
    // it any server is accepted.
    std::optional<crypto::PublicKey> expectedServer;
    std::size_t maxMessageBytes = 0;
};

// Wraps a listening endpoint. Connected is reported when the transport
// connects (the game server's Hello timeout covers the handshake); messages
// from a client only after its handshake, and sending to it fails until
// then. A client that sends anything else first (an older player) gets the
// game protocol's Reject (VersionMismatch) and is disconnected.
[[nodiscard]] std::unique_ptr<INetEndpoint> secureServer(std::unique_ptr<INetEndpoint> inner,
                                                         SecureServerOptions options);
// Wraps a client endpoint: Connected is reported once the server answered
// the handshake (and proved the expected identity).
[[nodiscard]] std::unique_ptr<INetEndpoint> secureClient(std::unique_ptr<INetEndpoint> inner,
                                                         SecureClientOptions options);

// Server identity keys as text (base64url of the 32-byte public key), for
// tickets, logs and --server-key.
[[nodiscard]] std::string encodeServerKey(const crypto::PublicKey& key);
[[nodiscard]] Result<crypto::PublicKey> decodeServerKey(std::string_view text);
// A secret identity key as text (base64url of the 32-byte secret), for
// handing to a server process (an environment variable) or a key file.
[[nodiscard]] std::string encodeIdentity(const crypto::KeyExchangeKey& key);
[[nodiscard]] Result<crypto::KeyExchangeKey> decodeIdentity(std::string_view text);

} // namespace px::net
