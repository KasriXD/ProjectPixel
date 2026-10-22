#include "px/core/Color.hpp"

#include <format>

namespace px {
namespace {

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::optional<std::uint8_t> parseByte(std::string_view two) {
    const int hi = hexValue(two[0]);
    const int lo = hexValue(two[1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    return static_cast<std::uint8_t>(hi * 16 + lo);
}

} // namespace

std::optional<Color> Color::parseHex(std::string_view text) {
    if (!text.empty() && text.front() == '#') text.remove_prefix(1);
    if (text.size() != 6 && text.size() != 8) return std::nullopt;
    Color c;
    const auto r = parseByte(text.substr(0, 2));
    const auto g = parseByte(text.substr(2, 2));
    const auto b = parseByte(text.substr(4, 2));
    if (!r || !g || !b) return std::nullopt;
    c.r = *r;
    c.g = *g;
    c.b = *b;
    if (text.size() == 8) {
        const auto a = parseByte(text.substr(6, 2));
        if (!a) return std::nullopt;
        c.a = *a;
    }
    return c;
}

std::string Color::toHex() const {
    if (a == 255) return std::format("#{:02x}{:02x}{:02x}", r, g, b);
    return std::format("#{:02x}{:02x}{:02x}{:02x}", r, g, b, a);
}

} // namespace px
