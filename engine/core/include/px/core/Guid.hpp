#pragma once

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace px {

// 128-bit globally unique identifier used for stable identities that are
// persisted in files: assets, scenes, entities, projects.
// Text form: lowercase "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".
class Guid {
public:
    constexpr Guid() = default;
    constexpr Guid(std::uint64_t high, std::uint64_t low) : m_high(high), m_low(low) {}

    // Random (version 4) GUID. Not cryptographically secure; never use as a secret.
    [[nodiscard]] static Guid generate();

    // Accepts the canonical 8-4-4-4-12 form or 32 bare hex digits.
    [[nodiscard]] static std::optional<Guid> parse(std::string_view text);

    [[nodiscard]] std::string toString() const;

    [[nodiscard]] constexpr bool isNil() const { return m_high == 0 && m_low == 0; }
    [[nodiscard]] constexpr std::uint64_t high() const { return m_high; }
    [[nodiscard]] constexpr std::uint64_t low() const { return m_low; }

    friend constexpr auto operator<=>(const Guid&, const Guid&) = default;

private:
    std::uint64_t m_high = 0;
    std::uint64_t m_low = 0;
};

} // namespace px

template <>
struct std::hash<px::Guid> {
    std::size_t operator()(const px::Guid& g) const noexcept {
        const std::uint64_t h = g.high() ^ (g.low() + 0x9e3779b97f4a7c15ULL + (g.high() << 6) + (g.high() >> 2));
        return static_cast<std::size_t>(h);
    }
};
