#pragma once

#include "px/core/Math.hpp"
#include "px/runtime/Input.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// On-screen controls for touch screens (phones, tablets, touch laptops):
// a direction pad for the move_* actions and a round button for each other
// action, laid out from the project's actions. Pure layout and hit testing,
// in the game's virtual pixels; the player draws them (docs/input-devices.md).
namespace px::runtime {

struct TouchButton {
    std::string label;          // "JUMP", or an arrow
    std::uint32_t actions = 0;  // bit mask
    Rect area;                  // what is drawn
    bool round = false;
    bool arrow = false;         // a direction of the pad
};

class TouchControls {
public:
    TouchControls() = default;
    TouchControls(const ActionSet& actions, Vec2 screen);

    // Actions held by touches at these points (virtual pixels).
    [[nodiscard]] std::uint32_t heldActions(std::span<const Vec2> touches) const;
    [[nodiscard]] const std::vector<TouchButton>& buttons() const { return m_buttons; }
    // The pause button (opens the settings menu), top right.
    [[nodiscard]] const Rect& pauseButton() const { return m_pause; }
    [[nodiscard]] bool hitsPause(Vec2 p) const;

private:
    std::vector<TouchButton> m_buttons;
    Rect m_pause;
    Vec2 m_padCenter;
    float m_padRadius = 0.0f;
    std::uint32_t m_left = 0, m_right = 0, m_up = 0, m_down = 0;
    float m_slack = 0.0f; // touches this far outside a button still count
};

} // namespace px::runtime
