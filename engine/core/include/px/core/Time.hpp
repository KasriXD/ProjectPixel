#pragma once

namespace px {

// Converts variable real frame time into a whole number of fixed simulation
// steps. Gameplay always advances in fixed steps so that simulation is
// deterministic and identical on client and server.
class FixedStepClock {
public:
    // stepSeconds > 0. maxStepsPerAdvance bounds catch-up work after a stall;
    // time beyond that is dropped (the game slows down instead of freezing).
    explicit FixedStepClock(double stepSeconds, int maxStepsPerAdvance = 8);

    // Adds elapsed real time and returns how many fixed steps to run now.
    [[nodiscard]] int advance(double elapsedSeconds);

    // Fraction (0..1) of a step left in the accumulator, for render interpolation.
    [[nodiscard]] double alpha() const { return m_accumulator / m_step; }
    [[nodiscard]] double step() const { return m_step; }
    void reset() { m_accumulator = 0.0; }

private:
    double m_step;
    double m_accumulator = 0.0;
    int m_maxSteps;
};

} // namespace px
