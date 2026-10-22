#include "px/core/Guid.hpp"

#include <chrono>
#include <format>
#include <random>

namespace px {
namespace {

std::mt19937_64& generator() {
    thread_local std::mt19937_64 gen = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(),
                          static_cast<unsigned>(std::chrono::steady_clock::now().time_since_epoch().count())};
        return std::mt19937_64(seq);
    }();
    return gen;
}

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

Guid Guid::generate() {
    auto& gen = generator();
    std::uint64_t high = gen();
    std::uint64_t low = gen();
    // RFC 9562 version 4 and variant bits.
    high = (high & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    low = (low & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    return Guid(high, low);
}

std::optional<Guid> Guid::parse(std::string_view text) {
    std::uint64_t parts[2] = {0, 0};
    int digits = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '-') {
            // Hyphens only at canonical positions.
            if (text.size() != 36 || (i != 8 && i != 13 && i != 18 && i != 23)) return std::nullopt;
            continue;
        }
        const int v = hexValue(c);
        if (v < 0 || digits >= 32) return std::nullopt;
        auto& part = parts[digits / 16];
        part = (part << 4) | static_cast<std::uint64_t>(v);
        ++digits;
    }
    if (digits != 32) return std::nullopt;
    if (text.size() != 32 && text.size() != 36) return std::nullopt;
    return Guid(parts[0], parts[1]);
}

std::string Guid::toString() const {
    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
                       static_cast<std::uint32_t>(m_high >> 32),
                       static_cast<std::uint32_t>((m_high >> 16) & 0xFFFF),
                       static_cast<std::uint32_t>(m_high & 0xFFFF),
                       static_cast<std::uint32_t>(m_low >> 48),
                       m_low & 0xFFFFFFFFFFFFULL);
}

} // namespace px
