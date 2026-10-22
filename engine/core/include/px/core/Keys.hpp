#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace px {

// Platform-independent physical key identifiers. Platform backends translate
// their native codes into these; project files refer to keys by name.
enum class Key : std::uint16_t {
    Unknown = 0,
    A, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Space, Enter, Escape, Backspace, Tab,
    Left, Right, Up, Down,
    LeftShift, RightShift, LeftCtrl, RightCtrl, LeftAlt, RightAlt,
    F1, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Count
};

inline constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::Count);

// Down/up state for every key, indexed by static_cast<std::size_t>(Key).
using KeyStates = std::array<bool, kKeyCount>;

// Canonical name, e.g. "A", "Space", "Left", "F1". Unknown -> "Unknown".
[[nodiscard]] std::string_view keyName(Key key) noexcept;

// Case-insensitive lookup by canonical name. Returns Key::Unknown if not found.
[[nodiscard]] Key keyFromName(std::string_view name) noexcept;

} // namespace px
