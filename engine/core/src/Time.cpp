#include "px/core/Time.hpp"

#include <algorithm>

namespace px {

FixedStepClock::FixedStepClock(double stepSeconds, int maxStepsPerAdvance)
    : m_step(stepSeconds > 0.0 ? stepSeconds : 1.0 / 60.0), m_maxSteps(std::max(1, maxStepsPerAdvance)) {}

int FixedStepClock::advance(double elapsedSeconds) {
    if (elapsedSeconds > 0.0) {
        m_accumulator += elapsedSeconds;
    }
    int steps = 0;
    while (m_accumulator >= m_step && steps < m_maxSteps) {
        m_accumulator -= m_step;
        ++steps;
    }
    if (steps == m_maxSteps && m_accumulator >= m_step) {
        // Too far behind: drop the backlog rather than spiralling.
        m_accumulator = 0.0;
    }
    return steps;
}

} // namespace px
