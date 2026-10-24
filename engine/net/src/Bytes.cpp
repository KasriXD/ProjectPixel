#include "px/net/Bytes.hpp"

#include <bit>
#include <cmath>
#include <cstring>

namespace px::net {

void ByteWriter::u16(std::uint16_t v) {
    u8(static_cast<std::uint8_t>(v));
    u8(static_cast<std::uint8_t>(v >> 8));
}

void ByteWriter::u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
}

void ByteWriter::u64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i)));
}

void ByteWriter::varint(std::uint64_t v) {
    while (v >= 0x80) {
        u8(static_cast<std::uint8_t>(v | 0x80));
        v >>= 7;
    }
    u8(static_cast<std::uint8_t>(v));
}

void ByteWriter::svarint(std::int64_t v) {
    const auto u = static_cast<std::uint64_t>(v);
    varint((u << 1) ^ static_cast<std::uint64_t>(v >> 63));
}

void ByteWriter::f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }

void ByteWriter::string(std::string_view s) {
    varint(s.size());
    m_data.insert(m_data.end(), s.begin(), s.end());
}

void ByteWriter::guid(const Guid& g) {
    u64(g.high());
    u64(g.low());
}

void ByteWriter::bytes(std::span<const std::uint8_t> data) { m_data.insert(m_data.end(), data.begin(), data.end()); }

bool ByteReader::need(std::size_t n) {
    if (m_failed || m_data.size() - m_pos < n) {
        m_failed = true;
        return false;
    }
    return true;
}

std::uint8_t ByteReader::u8() {
    if (!need(1)) return 0;
    return m_data[m_pos++];
}

std::uint16_t ByteReader::u16() {
    if (!need(2)) return 0;
    const auto v = static_cast<std::uint16_t>(m_data[m_pos] | (m_data[m_pos + 1] << 8));
    m_pos += 2;
    return v;
}

std::uint32_t ByteReader::u32() {
    if (!need(4)) return 0;
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(m_data[m_pos + static_cast<std::size_t>(i)]) << (8 * i);
    m_pos += 4;
    return v;
}

std::uint64_t ByteReader::u64() {
    if (!need(8)) return 0;
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(m_data[m_pos + static_cast<std::size_t>(i)]) << (8 * i);
    m_pos += 8;
    return v;
}

std::uint64_t ByteReader::varint() {
    std::uint64_t v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        const std::uint8_t b = u8();
        if (m_failed) return 0;
        // The tenth byte may only carry the top bit.
        if (shift == 63 && b > 1) {
            m_failed = true;
            return 0;
        }
        v |= static_cast<std::uint64_t>(b & 0x7f) << shift;
        if ((b & 0x80) == 0) return v;
    }
    m_failed = true;
    return 0;
}

std::int64_t ByteReader::svarint() {
    const std::uint64_t u = varint();
    return static_cast<std::int64_t>(u >> 1) ^ -static_cast<std::int64_t>(u & 1);
}

float ByteReader::f32() {
    const float v = std::bit_cast<float>(u32());
    if (!std::isfinite(v)) {
        m_failed = true;
        return 0.0f;
    }
    return v;
}

bool ByteReader::boolean() {
    const std::uint8_t v = u8();
    if (v > 1) m_failed = true;
    return v == 1;
}

std::string ByteReader::string(std::size_t maxLength) {
    const std::uint64_t length = varint();
    if (m_failed || length > maxLength || !need(static_cast<std::size_t>(length))) {
        m_failed = true;
        return {};
    }
    std::string s(reinterpret_cast<const char*>(m_data.data() + m_pos), static_cast<std::size_t>(length));
    m_pos += static_cast<std::size_t>(length);
    return s;
}

Guid ByteReader::guid() {
    const std::uint64_t high = u64();
    const std::uint64_t low = u64();
    return Guid(high, low);
}

std::size_t ByteReader::count(std::size_t max, std::size_t minBytesEach) {
    const std::uint64_t n = varint();
    if (m_failed || n > max || (minBytesEach > 0 && n > remaining() / minBytesEach)) {
        m_failed = true;
        return 0;
    }
    return static_cast<std::size_t>(n);
}

std::span<const std::uint8_t> ByteReader::bytes(std::size_t n) {
    if (!need(n)) return {};
    const auto out = m_data.subspan(m_pos, n);
    m_pos += n;
    return out;
}

void Fnv1a64::add(std::span<const std::uint8_t> data) {
    for (const std::uint8_t b : data) {
        m_hash ^= b;
        m_hash *= 0x100000001b3ULL;
    }
}

void Fnv1a64::add(std::string_view text) {
    add(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    addU64(text.size()); // so "ab"+"c" differs from "a"+"bc"
}

void Fnv1a64::addU64(std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        m_hash ^= static_cast<std::uint8_t>(v >> (8 * i));
        m_hash *= 0x100000001b3ULL;
    }
}

} // namespace px::net
