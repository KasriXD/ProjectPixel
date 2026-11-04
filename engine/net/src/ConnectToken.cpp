#include "px/net/ConnectToken.hpp"

#include "px/crypto/Encoding.hpp"

#include <cctype>
#include <chrono>
#include <charconv>
#include <format>
#include <vector>

namespace px::net {

namespace {
constexpr std::string_view kPrefix = "pxct1";

bool allOf(std::string_view s, bool (*ok)(char)) {
    for (const char c : s) {
        if (!ok(c)) return false;
    }
    return true;
}
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isNameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }
bool isTokenChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-'; }

std::string payloadOf(const ConnectToken& t) {
    return std::format("{}\n{}\n{}\n{}\n{}\n{}", kPrefix, t.session, t.userId, t.username, t.expires, t.nonce);
}

Error invalid(std::string_view why) {
    return Error{ErrorCode::AccessDenied, std::format("The join ticket is not valid ({}); join through the platform", why)};
}
} // namespace

std::string signConnectToken(const ConnectToken& token, const crypto::SigningKey& key) {
    const std::string payload = payloadOf(token);
    const crypto::Signature signature = crypto::sign(key, crypto::bytesOf(payload));
    return crypto::base64UrlEncode(crypto::bytesOf(payload)) + "." + crypto::base64UrlEncode(signature);
}

Result<ConnectToken> verifyConnectToken(std::string_view text, const crypto::PublicKey& key, std::int64_t now) {
    if (text.empty() || text.size() > kMaxConnectTokenLength) return invalid("missing or too long");
    const std::size_t dot = text.find('.');
    if (dot == std::string_view::npos) return invalid("malformed");
    const auto payload = crypto::base64UrlDecode(text.substr(0, dot));
    const auto signatureBytes = crypto::base64UrlDecode(text.substr(dot + 1));
    if (!payload || !signatureBytes || signatureBytes->size() != 64) return invalid("malformed");
    crypto::Signature signature{};
    std::copy(signatureBytes->begin(), signatureBytes->end(), signature.begin());
    if (!crypto::verify(key, *payload, signature)) return invalid("not signed by this platform");

    const std::string body(payload->begin(), payload->end());
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = body.find('\n', start);
        fields.emplace_back(std::string_view(body).substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (fields.size() != 6 || fields[0] != kPrefix) return invalid("unknown format");
    ConnectToken t;
    t.session = fields[1];
    t.userId = fields[2];
    t.username = fields[3];
    t.nonce = fields[5];
    const auto [p, ec] = std::from_chars(fields[4].data(), fields[4].data() + fields[4].size(), t.expires);
    if (ec != std::errc() || p != fields[4].data() + fields[4].size()) return invalid("bad expiry");
    if (t.session.empty() || t.session.size() > 64 || !allOf(t.session, isTokenChar) || t.userId.empty() ||
        t.userId.size() > 20 || !allOf(t.userId, isDigit) || t.username.empty() || t.username.size() > 32 ||
        !allOf(t.username, isNameChar) || t.nonce.size() < 8 || t.nonce.size() > 64 || !allOf(t.nonce, isTokenChar)) {
        return invalid("bad fields");
    }
    if (t.expires < now) return invalid("expired");
    if (t.expires > now + 24 * 3600) return invalid("expires too late");
    return t;
}

std::int64_t unixTimeNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace px::net
