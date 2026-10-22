#include "px/runtime/InputScript.hpp"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <string>

namespace px::runtime {

Result<InputScript> InputScript::parse(std::string_view text, const ActionSet& actions) {
    InputScript script;
    std::istringstream in{std::string(text)};
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream lineIn(line);
        std::string ticksText;
        std::string actionsText;
        if (!(lineIn >> ticksText)) continue; // blank line
        if (!(lineIn >> actionsText)) {
            return makeError(ErrorCode::InvalidData, "input script line {}: expected '<ticks> <actions>'", lineNumber);
        }
        std::string extra;
        if (lineIn >> extra) {
            return makeError(ErrorCode::InvalidData, "input script line {}: unexpected '{}' (join actions with '+')",
                             lineNumber, extra);
        }
        InputScriptStep step;
        const auto [ptr, ec] = std::from_chars(ticksText.data(), ticksText.data() + ticksText.size(), step.ticks);
        if (ec != std::errc() || ptr != ticksText.data() + ticksText.size() || step.ticks < 1 || step.ticks > 1000000) {
            return makeError(ErrorCode::InvalidData, "input script line {}: '{}' is not a valid tick count", lineNumber,
                             ticksText);
        }
        if (actionsText != "-") {
            std::size_t start = 0;
            while (start <= actionsText.size()) {
                const std::size_t end = std::min(actionsText.find('+', start), actionsText.size());
                const std::string_view name = std::string_view(actionsText).substr(start, end - start);
                const ActionId id = actions.find(name);
                if (id == kInvalidAction) {
                    return makeError(ErrorCode::InvalidData, "input script line {}: unknown action '{}'", lineNumber,
                                     name);
                }
                step.held |= (1u << id);
                start = end + 1;
            }
        }
        script.m_steps.push_back(step);
    }
    return script;
}

int InputScript::totalTicks() const {
    int total = 0;
    for (const auto& step : m_steps) total += step.ticks;
    return total;
}

InputFrame InputScriptPlayer::next() {
    std::uint32_t held = 0;
    if (!finished()) {
        held = m_script.steps()[m_step].held;
        if (++m_tickInStep >= m_script.steps()[m_step].ticks) {
            ++m_step;
            m_tickInStep = 0;
        }
    }
    const InputFrame frame = InputFrame::fromHeld(held, m_previousHeld);
    m_previousHeld = held;
    return frame;
}

} // namespace px::runtime
