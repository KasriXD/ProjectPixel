#pragma once

#include "px/core/Result.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Cryptographic building blocks (Monocypher underneath; docs/platform.md#security).
// Nothing here is game-visible; scripts have no access to it.
namespace px::crypto {

// --- Randomness ---------------------------------------------------------------------------
// Fills `out` from the operating system's secure random generator
// (getrandom, arc4random_buf, getentropy, BCryptGenRandom). Fails only if the OS does.
[[nodiscard]] Status randomBytes(std::span<std::uint8_t> out);
// A random secret of `bytes` bytes as base64url text (session tokens, server keys).
[[nodiscard]] Result<std::string> randomToken(std::size_t bytes = 32);

// --- Hashing ------------------------------------------------------------------------------
using Hash256 = std::array<std::uint8_t, 32>;
// BLAKE2b with a 32-byte digest.
[[nodiscard]] Hash256 hash256(std::span<const std::uint8_t> data);
[[nodiscard]] Hash256 hash256(std::string_view text);
// Incremental BLAKE2b-256 for large or streamed data.
class Hasher {
public:
    Hasher();
    void update(std::span<const std::uint8_t> data);
    [[nodiscard]] Hash256 finish();

private:
    alignas(8) std::array<std::uint8_t, 256> m_state{};
};

// Comparison whose time does not depend on where the inputs differ.
[[nodiscard]] bool constantTimeEqual(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b);

// --- Passwords ----------------------------------------------------------------------------
// Argon2id. The parameters are stored in the hash, so they can be raised
// later without invalidating existing passwords.
struct PasswordHashing {
    std::uint32_t memoryKiB = 19 * 1024; // OWASP's minimum for Argon2id
    std::uint32_t passes = 2;
};
// "$argon2id$v=19$m=19456,t=2,p=1$<salt>$<hash>" (salt and hash in base64url).
[[nodiscard]] Result<std::string> hashPassword(std::string_view password, const PasswordHashing& params = {});
[[nodiscard]] bool verifyPassword(std::string_view password, std::string_view stored);

// --- Signatures ---------------------------------------------------------------------------
// EdDSA over Curve25519 with BLAKE2b (Monocypher's crypto_eddsa).
using PublicKey = std::array<std::uint8_t, 32>;
using Signature = std::array<std::uint8_t, 64>;
struct SigningKey {
    std::array<std::uint8_t, 64> secret{}; // keep private
    PublicKey publicKey{};

    [[nodiscard]] static Result<SigningKey> generate();
    // From the 32-byte seed it was generated from (for storing the key).
    [[nodiscard]] static SigningKey fromSeed(std::span<const std::uint8_t, 32> seed);
};
[[nodiscard]] Signature sign(const SigningKey& key, std::span<const std::uint8_t> message);
[[nodiscard]] bool verify(const PublicKey& key, std::span<const std::uint8_t> message, const Signature& signature);

// Overwrites memory that held a secret (not optimized away).
void wipe(std::span<std::uint8_t> secret);

} // namespace px::crypto
