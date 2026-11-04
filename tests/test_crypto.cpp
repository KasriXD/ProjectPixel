#include "px/crypto/Crypto.hpp"
#include "px/crypto/Encoding.hpp"

#include <doctest.h>

#include <set>

using namespace px;
using namespace px::crypto;

TEST_CASE("Crypto: hex and base64url round-trip and reject bad text") {
    const std::vector<std::uint8_t> data = {0, 1, 2, 250, 255, 16, 32};
    CHECK(toHex(data) == "000102faff1020");
    CHECK(fromHex("000102FAff1020") == data);
    CHECK_FALSE(fromHex("abc").has_value());
    CHECK_FALSE(fromHex("zz").has_value());
    for (std::size_t n = 0; n <= data.size(); ++n) {
        const std::span<const std::uint8_t> part(data.data(), n);
        const std::string text = base64UrlEncode(part);
        CHECK(text.find('=') == std::string::npos);
        const auto back = base64UrlDecode(text);
        REQUIRE(back.has_value());
        CHECK(std::vector<std::uint8_t>(part.begin(), part.end()) == *back);
    }
    CHECK(base64UrlEncode(bytesOf("hi?>")) == "aGk_Pg");
    CHECK_FALSE(base64UrlDecode("a").has_value());      // impossible length
    CHECK_FALSE(base64UrlDecode("aGk+Pg").has_value()); // standard base64 characters
    CHECK_FALSE(base64UrlDecode("aB").has_value());     // non-zero leftover bits
}

TEST_CASE("Crypto: random tokens differ and hashes are BLAKE2b-256") {
    std::set<std::string> seen;
    for (int i = 0; i < 50; ++i) {
        auto t = randomToken(32);
        REQUIRE(t.ok());
        CHECK(t.value().size() == 43);
        seen.insert(t.value());
    }
    CHECK(seen.size() == 50);
    // RFC 7693 does not list BLAKE2b-256("abc"); this is the reference value.
    CHECK(toHex(hash256(std::string_view("abc"))) ==
          "bddd813c634239723171ef3fee98579b94964e3bb1cb3e427262c8c068d52319");
    Hasher h;
    h.update(bytesOf("a"));
    h.update(bytesOf("bc"));
    CHECK(h.finish() == hash256(std::string_view("abc")));
    CHECK(constantTimeEqual(bytesOf("same"), bytesOf("same")));
    CHECK_FALSE(constantTimeEqual(bytesOf("same"), bytesOf("sane")));
    CHECK_FALSE(constantTimeEqual(bytesOf("same"), bytesOf("same!")));
}

TEST_CASE("Crypto: passwords hash with Argon2id and verify") {
    const PasswordHashing fast{64, 1}; // tests only; the default is 19 MiB, 2 passes
    auto stored = hashPassword("correct horse", fast);
    REQUIRE(stored.ok());
    CHECK(stored.value().starts_with("$argon2id$v=19$m=64,t=1,p=1$"));
    CHECK(verifyPassword("correct horse", stored.value()));
    CHECK_FALSE(verifyPassword("correct horsE", stored.value()));
    CHECK_FALSE(verifyPassword("", stored.value()));
    auto again = hashPassword("correct horse", fast);
    REQUIRE(again.ok());
    CHECK(again.value() != stored.value()); // salted
    // Tampered or hostile stored hashes are rejected, not trusted.
    CHECK_FALSE(verifyPassword("x", "$argon2id$v=19$m=99999999,t=1,p=1$AAAAAAAAAAAAAAAAAAAAAA$AAAA"));
    CHECK_FALSE(verifyPassword("x", "plain"));
    CHECK_FALSE(verifyPassword("x", "$argon2i$v=19$m=64,t=1,p=1$a$b"));
    CHECK_FALSE(hashPassword(std::string(2000, 'x'), fast).ok());
}

TEST_CASE("Crypto: signatures verify only with the right key and message") {
    auto key = SigningKey::generate();
    REQUIRE(key.ok());
    const auto message = bytesOf("session=1;player=ada");
    const Signature sig = sign(key.value(), message);
    CHECK(verify(key.value().publicKey, message, sig));
    CHECK_FALSE(verify(key.value().publicKey, bytesOf("session=1;player=bob"), sig));
    Signature bad = sig;
    bad[10] ^= 1;
    CHECK_FALSE(verify(key.value().publicKey, message, bad));
    auto other = SigningKey::generate();
    REQUIRE(other.ok());
    CHECK_FALSE(verify(other.value().publicKey, message, sig));
    // A key restored from its seed is the same key.
    const std::span<const std::uint8_t, 32> seed(key.value().secret.data(), 32);
    const SigningKey restored = SigningKey::fromSeed(seed);
    CHECK(restored.publicKey == key.value().publicKey);
    CHECK(verify(restored.publicKey, message, sign(restored, message)));
}
