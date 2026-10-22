#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace px {

// 8-bit-per-channel RGBA color (non-premultiplied).
struct Color {
    std::uint8_t r = 255;
    std::uint8_t g = 255;
    std::uint8_t b = 255;
    std::uint8_t a = 255;

    constexpr bool operator==(const Color&) const = default;

    // Parses "#RRGGBB" or "#RRGGBBAA" (case-insensitive, '#' optional).
    [[nodiscard]] static std::optional<Color> parseHex(std::string_view text);
    // "#rrggbb" when opaque, otherwise "#rrggbbaa".
    [[nodiscard]] std::string toHex() const;
};

namespace colors {
inline constexpr Color white{255, 255, 255, 255};
inline constexpr Color black{0, 0, 0, 255};
inline constexpr Color transparent{0, 0, 0, 0};
inline constexpr Color magenta{255, 0, 255, 255};
inline constexpr Color red{230, 60, 60, 255};
inline constexpr Color green{60, 200, 90, 255};
inline constexpr Color yellow{250, 210, 60, 255};
inline constexpr Color cyan{80, 220, 230, 255};
} // namespace colors

} // namespace px
