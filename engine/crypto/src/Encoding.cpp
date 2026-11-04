#include "px/crypto/Encoding.hpp"

#include <array>

namespace px::crypto {

namespace {
constexpr char kHex[] = "0123456789abcdef";
constexpr char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}
} // namespace

std::string toHex(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const std::uint8_t b : bytes) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> fromHex(std::string_view text) {
    if (text.size() % 2 != 0) return std::nullopt;
    std::vector<std::uint8_t> out(text.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = hexValue(text[2 * i]);
        const int lo = hexValue(text[2 * i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out[i] = static_cast<std::uint8_t>(hi * 16 + lo);
    }
    return out;
}

std::string base64UrlEncode(std::span<const std::uint8_t> bytes) {
    std::string out;
    out.reserve((bytes.size() * 4 + 2) / 3);
    std::size_t i = 0;
    for (; i + 3 <= bytes.size(); i += 3) {
        const std::uint32_t v = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8) | bytes[i + 2];
        out += kBase64Url[(v >> 18) & 63];
        out += kBase64Url[(v >> 12) & 63];
        out += kBase64Url[(v >> 6) & 63];
        out += kBase64Url[v & 63];
    }
    const std::size_t rest = bytes.size() - i;
    if (rest == 1) {
        const std::uint32_t v = std::uint32_t{bytes[i]} << 16;
        out += kBase64Url[(v >> 18) & 63];
        out += kBase64Url[(v >> 12) & 63];
    } else if (rest == 2) {
        const std::uint32_t v = (std::uint32_t{bytes[i]} << 16) | (std::uint32_t{bytes[i + 1]} << 8);
        out += kBase64Url[(v >> 18) & 63];
        out += kBase64Url[(v >> 12) & 63];
        out += kBase64Url[(v >> 6) & 63];
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base64UrlDecode(std::string_view text) {
    if (text.size() % 4 == 1) return std::nullopt;
    std::vector<std::uint8_t> out;
    out.reserve(text.size() * 3 / 4);
    std::uint32_t buffer = 0;
    int bits = 0;
    for (const char c : text) {
        const int v = base64Value(c);
        if (v < 0) return std::nullopt;
        buffer = (buffer << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    // Leftover bits must be zero (canonical encoding only).
    if (bits > 0 && (buffer & ((1u << bits) - 1u)) != 0) return std::nullopt;
    return out;
}

} // namespace px::crypto
