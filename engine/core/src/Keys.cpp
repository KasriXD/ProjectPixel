#include "px/core/Keys.hpp"

#include <array>
#include <cctype>

namespace px {
namespace {

constexpr std::array<std::string_view, kKeyCount> kNames = {
    "Unknown",
    "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M",
    "N", "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "Space", "Enter", "Escape", "Backspace", "Tab",
    "Left", "Right", "Up", "Down",
    "LeftShift", "RightShift", "LeftCtrl", "RightCtrl", "LeftAlt", "RightAlt",
    "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12",
};

bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::string_view keyName(Key key) noexcept {
    const auto index = static_cast<std::size_t>(key);
    return index < kNames.size() ? kNames[index] : kNames[0];
}

Key keyFromName(std::string_view name) noexcept {
    for (std::size_t i = 1; i < kNames.size(); ++i) {
        if (equalsIgnoreCase(kNames[i], name)) return static_cast<Key>(i);
    }
    return Key::Unknown;
}

std::string_view padButtonName(PadButton button) noexcept {
    switch (button) {
        case PadButton::South: return "South (A)";
        case PadButton::East: return "East (B)";
        case PadButton::West: return "West (X)";
        case PadButton::North: return "North (Y)";
        case PadButton::Back: return "Back";
        case PadButton::Start: return "Start";
        case PadButton::LeftShoulder: return "Left shoulder";
        case PadButton::RightShoulder: return "Right shoulder";
        case PadButton::DPadUp: return "D-pad up";
        case PadButton::DPadDown: return "D-pad down";
        case PadButton::DPadLeft: return "D-pad left";
        case PadButton::DPadRight: return "D-pad right";
        case PadButton::Count: break;
    }
    return "?";
}

} // namespace px
