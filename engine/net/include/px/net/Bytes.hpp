#pragma once

#include "px/core/Guid.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Little-endian binary encoding for network messages. Readers never trust
// their input: every read is bounds-checked, and the first failure makes the
// reader "failed" for good (later reads return zeros), so decoders can read
// a whole message and check ok() once.
namespace px::net {

class ByteWriter {
public:
    void u8(std::uint8_t v) { m_data.push_back(v); }
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void varint(std::uint64_t v);   // 1-10 bytes, small values are short
    void svarint(std::int64_t v);   // zigzag + varint
    void f32(float v);              // IEEE 754 bits
    void boolean(bool v) { u8(v ? 1 : 0); }
    void string(std::string_view s); // varint length + bytes
    void guid(const Guid& g);
    void bytes(std::span<const std::uint8_t> data);

    [[nodiscard]] const std::vector<std::uint8_t>& data() const { return m_data; }
    [[nodiscard]] std::vector<std::uint8_t>& data() { return m_data; }
    [[nodiscard]] std::size_t size() const { return m_data.size(); }

private:
    std::vector<std::uint8_t> m_data;
};

class ByteReader {
public:
    explicit ByteReader(std::span<const std::uint8_t> data) : m_data(data) {}

    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    std::uint64_t varint();
    std::int64_t svarint();
    // Rejects NaN and infinities (they would poison positions and physics).
    float f32();
    bool boolean();
    std::string string(std::size_t maxLength);
    Guid guid();
    // A varint count that must be at most `max` and could fit in the rest of
    // the message at `minBytesEach` bytes per item (blocks huge allocations).
    std::size_t count(std::size_t max, std::size_t minBytesEach = 1);
    std::span<const std::uint8_t> bytes(std::size_t n);
    // The bytes from position `from` up to the current position.
    [[nodiscard]] std::span<const std::uint8_t> span(std::size_t from) const {
        return from <= m_pos ? m_data.subspan(from, m_pos - from) : std::span<const std::uint8_t>{};
    }

    [[nodiscard]] bool ok() const { return !m_failed; }
    [[nodiscard]] bool atEnd() const { return m_pos == m_data.size(); }
    [[nodiscard]] std::size_t remaining() const { return m_failed ? 0 : m_data.size() - m_pos; }
    [[nodiscard]] std::size_t position() const { return m_pos; }
    void fail() { m_failed = true; }

private:
    [[nodiscard]] bool need(std::size_t n);

    std::span<const std::uint8_t> m_data;
    std::size_t m_pos = 0;
    bool m_failed = false;
};

// FNV-1a, 64-bit: a fast non-cryptographic hash for comparing content.
class Fnv1a64 {
public:
    void add(std::span<const std::uint8_t> data);
    void add(std::string_view text);
    void addU64(std::uint64_t v);
    [[nodiscard]] std::uint64_t value() const { return m_hash; }

private:
    std::uint64_t m_hash = 0xcbf29ce484222325ULL;
};

} // namespace px::net
