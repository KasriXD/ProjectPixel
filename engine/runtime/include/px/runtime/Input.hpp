#pragma once

#include "px/core/Keys.hpp"
#include "px/project/Project.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Gameplay never sees physical keys, only named actions ("jump"). Clients map
// devices to actions; the simulation consumes one InputFrame per tick. The
// same frames will be sent to the authoritative server in multiplayer.
namespace px::runtime {

using ActionId = std::uint8_t;
inline constexpr ActionId kInvalidAction = 0xFF;
inline constexpr std::size_t kMaxActions = 32;

// Standard action names used by built-in components.
namespace actions {
inline constexpr std::string_view kMoveLeft = "move_left";
inline constexpr std::string_view kMoveRight = "move_right";
inline constexpr std::string_view kMoveUp = "move_up";
inline constexpr std::string_view kMoveDown = "move_down";
inline constexpr std::string_view kJump = "jump";
} // namespace actions

// Maps action names to dense ids (bit positions in an InputFrame).
class ActionSet {
public:
    ActionSet() = default;
    explicit ActionSet(const project::InputConfig& config);

    [[nodiscard]] ActionId find(std::string_view name) const;
    [[nodiscard]] std::string_view name(ActionId id) const;
    [[nodiscard]] std::size_t size() const { return m_names.size(); }

private:
    std::vector<std::string> m_names;
};

// Input for a single simulation tick.
struct InputFrame {
    std::uint32_t held = 0;    // bit i set: action i is held
    std::uint32_t pressed = 0; // bit i set: action i went down since the previous tick

    [[nodiscard]] bool isHeld(ActionId a) const { return a < kMaxActions && ((held >> a) & 1u) != 0; }
    [[nodiscard]] bool wasPressed(ActionId a) const { return a < kMaxActions && ((pressed >> a) & 1u) != 0; }

    // Builds a frame from held states, deriving presses from the previous tick.
    [[nodiscard]] static InputFrame fromHeld(std::uint32_t held, std::uint32_t previousHeld) {
        return {held, held & ~previousHeld};
    }
};

// Default keyboard bindings from the project file, resolved to action ids.
class KeyBindings {
public:
    KeyBindings() = default;
    KeyBindings(const project::InputConfig& config, const ActionSet& actionSet);

    // Bitmask of actions with at least one bound key down.
    [[nodiscard]] std::uint32_t heldActions(const KeyStates& keys) const;
    // Actions bound to `key` (bitmask).
    [[nodiscard]] std::uint32_t actionsForKey(Key key) const;

private:
    std::array<std::uint32_t, kKeyCount> m_keyToActions{};
};

} // namespace px::runtime
