#include "px/crypto/Crypto.hpp"

#include "px/crypto/Encoding.hpp"

#include "monocypher.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>
#include <memory>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__ANDROID__)
#include <cstdlib> // arc4random_buf (getrandom/getentropy: not on iOS, Android API 28+ only)
#elif defined(__linux__)
#include <sys/random.h>
#include <cerrno>
#else
#include <unistd.h> // getentropy (BSDs, Emscripten)
#endif

namespace px::crypto {

// --- Randomness ---------------------------------------------------------------------------

Status randomBytes(std::span<std::uint8_t> out) {
#if defined(_WIN32)
    std::size_t done = 0;
    while (done < out.size()) {
        const auto n = static_cast<ULONG>(std::min<std::size_t>(out.size() - done, 1u << 20));
        if (BCryptGenRandom(nullptr, out.data() + done, n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            return makeError(ErrorCode::IoError, "the system random number generator failed");
        }
        done += n;
    }
#elif defined(__APPLE__) || defined(__ANDROID__)
    // The kernel-seeded system generator; it cannot fail.
    arc4random_buf(out.data(), out.size());
#elif defined(__linux__)
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = getrandom(out.data() + done, out.size() - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return makeError(ErrorCode::IoError, "the system random number generator failed");
        }
        done += static_cast<std::size_t>(n);
    }
#else
    std::size_t done = 0;
    while (done < out.size()) {
        const std::size_t n = std::min<std::size_t>(out.size() - done, 256); // getentropy's limit
        if (getentropy(out.data() + done, n) != 0) {
            return makeError(ErrorCode::IoError, "the system random number generator failed");
        }
        done += n;
    }
#endif
    return {};
}

Result<std::string> randomToken(std::size_t bytes) {
    std::vector<std::uint8_t> buffer(bytes);
    if (auto s = randomBytes(buffer); !s) return s.error();
    std::string out = base64UrlEncode(buffer);
    wipe(buffer);
    return out;
}

// --- Hashing ------------------------------------------------------------------------------

Hash256 hash256(std::span<const std::uint8_t> data) {
    Hash256 out{};
    crypto_blake2b(out.data(), out.size(), data.data(), data.size());
    return out;
}

Hash256 hash256(std::string_view text) { return hash256(bytesOf(text)); }

static_assert(sizeof(crypto_blake2b_ctx) <= 256, "Hasher state is too small");

Hasher::Hasher() { crypto_blake2b_init(reinterpret_cast<crypto_blake2b_ctx*>(m_state.data()), 32); }

void Hasher::update(std::span<const std::uint8_t> data) {
    crypto_blake2b_update(reinterpret_cast<crypto_blake2b_ctx*>(m_state.data()), data.data(), data.size());
}

Hash256 Hasher::finish() {
    Hash256 out{};
    crypto_blake2b_final(reinterpret_cast<crypto_blake2b_ctx*>(m_state.data()), out.data());
    return out;
}

bool constantTimeEqual(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) {
    if (a.size() != b.size()) return false;
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) diff = static_cast<std::uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

void wipe(std::span<std::uint8_t> secret) { crypto_wipe(secret.data(), secret.size()); }

Hash256 keyedHash256(std::span<const std::uint8_t> key, std::span<const std::uint8_t> data) {
    Hash256 out{};
    crypto_blake2b_keyed(out.data(), out.size(), key.data(), std::min<std::size_t>(key.size(), 64), data.data(),
                         data.size());
    return out;
}

// --- Key exchange -------------------------------------------------------------------------

Result<KeyExchangeKey> KeyExchangeKey::generate() {
    std::array<std::uint8_t, 32> secret{};
    if (auto s = randomBytes(secret); !s) return s.error();
    KeyExchangeKey key = fromSecret(secret);
    crypto_wipe(secret.data(), secret.size());
    return key;
}

KeyExchangeKey KeyExchangeKey::fromSecret(std::span<const std::uint8_t, 32> secret) {
    KeyExchangeKey key;
    std::copy(secret.begin(), secret.end(), key.secret.begin());
    crypto_x25519_public_key(key.publicKey.data(), key.secret.data());
    return key;
}

std::optional<std::array<std::uint8_t, 32>> keyExchange(const KeyExchangeKey& mine, const PublicKey& theirs) {
    std::array<std::uint8_t, 32> shared{};
    crypto_x25519(shared.data(), mine.secret.data(), theirs.data());
    const std::array<std::uint8_t, 32> zero{};
    if (crypto_verify32(shared.data(), zero.data()) == 0) return std::nullopt; // a low-order point
    return shared;
}

// --- Authenticated encryption ---------------------------------------------------------------

void seal(std::span<std::uint8_t> cipher, Mac& mac, const SecretKey& key, const Nonce& nonce,
          std::span<const std::uint8_t> extra, std::span<const std::uint8_t> plain) {
    crypto_aead_lock(cipher.data(), mac.data(), key.data(), nonce.data(), extra.data(), extra.size(), plain.data(),
                     std::min(plain.size(), cipher.size()));
}

bool open(std::span<std::uint8_t> plain, const Mac& mac, const SecretKey& key, const Nonce& nonce,
          std::span<const std::uint8_t> extra, std::span<const std::uint8_t> cipher) {
    if (plain.size() < cipher.size()) return false;
    return crypto_aead_unlock(plain.data(), mac.data(), key.data(), nonce.data(), extra.data(), extra.size(),
                              cipher.data(), cipher.size()) == 0;
}

// --- Passwords ----------------------------------------------------------------------------

namespace {
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;
constexpr std::uint32_t kMaxMemoryKiB = 1024 * 1024; // refuse absurd stored parameters (1 GiB)
constexpr std::uint32_t kMaxPasses = 16;
constexpr std::size_t kMaxPasswordBytes = 1024;

std::vector<std::uint8_t> argon2id(std::string_view password, std::span<const std::uint8_t> salt, std::uint32_t memoryKiB,
                                   std::uint32_t passes) {
    std::vector<std::uint8_t> hash(kHashBytes);
    auto work = std::make_unique<std::uint8_t[]>(std::size_t{memoryKiB} * 1024);
    crypto_argon2_config config{CRYPTO_ARGON2_ID, memoryKiB, passes, 1};
    crypto_argon2_inputs inputs{reinterpret_cast<const std::uint8_t*>(password.data()), salt.data(),
                                static_cast<std::uint32_t>(password.size()), static_cast<std::uint32_t>(salt.size())};
    crypto_argon2(hash.data(), static_cast<std::uint32_t>(hash.size()), work.get(), config, inputs,
                  crypto_argon2_no_extras);
    crypto_wipe(work.get(), std::size_t{memoryKiB} * 1024);
    return hash;
}

bool parseUint(std::string_view text, std::uint32_t& out) {
    const auto [p, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && p == text.data() + text.size();
}
} // namespace

Result<std::string> hashPassword(std::string_view password, const PasswordHashing& params) {
    if (password.size() > kMaxPasswordBytes) return makeError(ErrorCode::InvalidArgument, "the password is too long");
    if (params.memoryKiB < 8 || params.memoryKiB > kMaxMemoryKiB || params.passes < 1 || params.passes > kMaxPasses) {
        return makeError(ErrorCode::InvalidArgument, "invalid password hashing parameters");
    }
    std::array<std::uint8_t, kSaltBytes> salt{};
    if (auto s = randomBytes(salt); !s) return s.error();
    const auto hash = argon2id(password, salt, params.memoryKiB, params.passes);
    return std::format("$argon2id$v=19$m={},t={},p=1${}${}", params.memoryKiB, params.passes, base64UrlEncode(salt),
                       base64UrlEncode(hash));
}

bool verifyPassword(std::string_view password, std::string_view stored) {
    if (password.size() > kMaxPasswordBytes) return false;
    // $argon2id$v=19$m=<m>,t=<t>,p=1$<salt>$<hash>
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= stored.size()) {
        const std::size_t end = stored.find('$', start);
        parts.push_back(stored.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (parts.size() != 6 || !parts[0].empty() || parts[1] != "argon2id" || parts[2] != "v=19") return false;
    const std::string_view p = parts[3];
    if (!p.starts_with("m=")) return false;
    const std::size_t comma1 = p.find(",t=");
    const std::size_t comma2 = p.find(",p=1");
    if (comma1 == std::string_view::npos || comma2 == std::string_view::npos || comma2 < comma1 ||
        comma2 + 4 != p.size()) {
        return false;
    }
    std::uint32_t memory = 0;
    std::uint32_t passes = 0;
    if (!parseUint(p.substr(2, comma1 - 2), memory) || !parseUint(p.substr(comma1 + 3, comma2 - comma1 - 3), passes)) {
        return false;
    }
    if (memory < 8 || memory > kMaxMemoryKiB || passes < 1 || passes > kMaxPasses) return false;
    const auto salt = base64UrlDecode(parts[4]);
    const auto expected = base64UrlDecode(parts[5]);
    if (!salt || !expected || salt->size() < 8 || expected->size() != kHashBytes) return false;
    const auto actual = argon2id(password, *salt, memory, passes);
    return constantTimeEqual(actual, *expected);
}

// --- Signatures ---------------------------------------------------------------------------

SigningKey SigningKey::fromSeed(std::span<const std::uint8_t, 32> seed) {
    SigningKey key;
    std::array<std::uint8_t, 32> copy{};
    std::memcpy(copy.data(), seed.data(), copy.size());
    crypto_eddsa_key_pair(key.secret.data(), key.publicKey.data(), copy.data()); // wipes `copy`
    return key;
}

Result<SigningKey> SigningKey::generate() {
    std::array<std::uint8_t, 32> seed{};
    if (auto s = randomBytes(seed); !s) return s.error();
    SigningKey key = fromSeed(seed);
    crypto_wipe(seed.data(), seed.size());
    return key;
}

Signature sign(const SigningKey& key, std::span<const std::uint8_t> message) {
    Signature out{};
    crypto_eddsa_sign(out.data(), key.secret.data(), message.data(), message.size());
    return out;
}

bool verify(const PublicKey& key, std::span<const std::uint8_t> message, const Signature& signature) {
    return crypto_eddsa_check(signature.data(), key.data(), message.data(), message.size()) == 0;
}

} // namespace px::crypto
