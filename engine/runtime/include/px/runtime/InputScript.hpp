#pragma once

#include "px/core/Result.hpp"
#include "px/runtime/Input.hpp"

#include <cstdint>
#include <string_view>
#include <vector>

// Scripted input for automated tests, headless runs and reproducible demos.
//
// Text format, one step per line:
//     <ticks> <action>[+<action>...]     hold these actions for <ticks> ticks
//     <ticks> -                          hold nothing for <ticks> ticks
// Blank lines and lines starting with '#' are ignored. Example:
//     30 move_right
//     1  move_right+jump
//     20 -
namespace px::runtime {

struct InputScriptStep {
    int ticks = 0;
    std::uint32_t held = 0;
};

class InputScript {
public:
    [[nodiscard]] static Result<InputScript> parse(std::string_view text, const ActionSet& actions);

    [[nodiscard]] const std::vector<InputScriptStep>& steps() const { return m_steps; }
    [[nodiscard]] int totalTicks() const;

private:
    std::vector<InputScriptStep> m_steps;
};

// Plays an InputScript one tick at a time, producing frames with correct
// press edges. After the script ends, frames are empty.
class InputScriptPlayer {
public:
    explicit InputScriptPlayer(InputScript script) : m_script(std::move(script)) {}

    [[nodiscard]] InputFrame next();
    [[nodiscard]] bool finished() const { return m_step >= m_script.steps().size(); }

private:
    InputScript m_script;
    std::size_t m_step = 0;
    int m_tickInStep = 0;
    std::uint32_t m_previousHeld = 0;
};

} // namespace px::runtime
