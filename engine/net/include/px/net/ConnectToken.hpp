#pragma once

#include "px/core/Result.hpp"
#include "px/crypto/Crypto.hpp"

#include <cstdint>
#include <string>
#include <string_view>

// Connect tokens: the platform's permission for one account to join one
// game server, for a short time (docs/platform.md#joining-a-game). The
// platform signs them with its private key; game servers check them with
// the platform's public key, so a server needs no connection to the
// platform to decide who may join, and nobody can join as someone else.
namespace px::net {

inline constexpr std::int64_t kConnectTokenLifetimeSeconds = 60;
inline constexpr std::size_t kMaxConnectTokenLength = 600;

struct ConnectToken {
    std::string session;  // the game server session it is valid for
    std::string userId;   // the platform account, as decimal text
    std::string username; // shown in game; verified, unlike names in Hello
    std::int64_t expires = 0; // Unix seconds
    std::string nonce;    // random; a server accepts each token once
};

// "<payload>.<signature>", both base64url.
[[nodiscard]] std::string signConnectToken(const ConnectToken& token, const crypto::SigningKey& key);
// Checks the signature, the fields and that the token has not expired at `now`.
[[nodiscard]] Result<ConnectToken> verifyConnectToken(std::string_view text, const crypto::PublicKey& key,
                                                      std::int64_t now);

// Seconds since 1970 (UTC), for token expiry. Not for gameplay.
[[nodiscard]] std::int64_t unixTimeNow();

} // namespace px::net
