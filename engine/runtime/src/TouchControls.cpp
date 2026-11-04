#include "px/runtime/TouchControls.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace px::runtime {

namespace {
std::uint32_t bit(const ActionSet& actions, std::string_view name) {
    const ActionId id = actions.find(name);
    return id == kInvalidAction ? 0u : (1u << id);
}

std::string labelFor(std::string_view name) {
    std::string out;
    for (const char c : name) {
        if (out.size() >= 5) break;
        out += c == '_' ? ' ' : static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool inside(const Rect& r, Vec2 p, float slack) {
    return p.x >= r.x - slack && p.y >= r.y - slack && p.x <= r.x + r.w + slack && p.y <= r.y + r.h + slack;
}
} // namespace

TouchControls::TouchControls(const ActionSet& actions, Vec2 screen) {
    const float s = std::clamp(screen.y * 0.15f, 16.0f, 96.0f); // button size
    const float margin = s * 0.4f;
    m_slack = s * 0.25f;
    m_left = bit(actions, actions::kMoveLeft);
    m_right = bit(actions, actions::kMoveRight);
    m_up = bit(actions, actions::kMoveUp);
    m_down = bit(actions, actions::kMoveDown);

    // Direction pad, bottom left: a cross when the game moves in four
    // directions, two arrows for side-view games.
    const bool vertical = m_up != 0 || m_down != 0;
    const float padSize = vertical ? 3.0f * s : 2.2f * s;
    m_padCenter = {margin + padSize * 0.5f, screen.y - margin - (vertical ? padSize * 0.5f : s * 0.5f)};
    m_padRadius = vertical ? padSize * 0.75f : padSize * 0.6f;
    const auto arrow = [&](const char* label, std::uint32_t mask, Vec2 center) {
        if (mask == 0) return;
        m_buttons.push_back({label, mask, {center.x - s * 0.5f, center.y - s * 0.5f, s, s}, false, true});
    };
    if (vertical) {
        arrow("<", m_left, {m_padCenter.x - s, m_padCenter.y});
        arrow(">", m_right, {m_padCenter.x + s, m_padCenter.y});
        arrow("^", m_up, {m_padCenter.x, m_padCenter.y - s});
        arrow("v", m_down, {m_padCenter.x, m_padCenter.y + s});
    } else {
        arrow("<", m_left, {m_padCenter.x - s * 0.6f, m_padCenter.y});
        arrow(">", m_right, {m_padCenter.x + s * 0.6f, m_padCenter.y});
    }

    // The other actions, bottom right: the first (usually jump) largest.
    std::vector<std::pair<std::string, std::uint32_t>> others;
    for (std::size_t i = 0; i < actions.size() && others.size() < 4; ++i) {
        const auto name = actions.name(static_cast<ActionId>(i));
        if (name == actions::kMoveLeft || name == actions::kMoveRight || name == actions::kMoveUp ||
            name == actions::kMoveDown) {
            continue;
        }
        others.emplace_back(labelFor(name), 1u << i);
    }
    for (std::size_t i = 0; i < others.size(); ++i) {
        const float size = i == 0 ? s * 1.25f : s;
        Vec2 c{screen.x - margin - s * 0.65f, screen.y - margin - s * 0.65f};
        if (i == 1) c = {c.x - s * 1.45f, c.y + s * 0.1f};
        if (i == 2) c = {c.x - s * 0.15f, c.y - s * 1.45f};
        if (i == 3) c = {c.x - s * 1.5f, c.y - s * 1.3f};
        m_buttons.push_back({others[i].first, others[i].second, {c.x - size * 0.5f, c.y - size * 0.5f, size, size}, true,
                             false});
    }
    const float p = s * 0.7f;
    m_pause = {screen.x - margin * 0.5f - p, margin * 0.5f, p, p};
}

bool TouchControls::hitsPause(Vec2 p) const { return m_pause.w > 0.0f && inside(m_pause, p, m_slack * 0.5f); }

std::uint32_t TouchControls::heldActions(std::span<const Vec2> touches) const {
    std::uint32_t held = 0;
    for (const Vec2 t : touches) {
        // The pad: the direction from its centre, so a thumb resting between
        // two arrows presses both (diagonals).
        const Vec2 d{t.x - m_padCenter.x, t.y - m_padCenter.y};
        const float length = std::sqrt(d.x * d.x + d.y * d.y);
        if (length <= m_padRadius && length > m_padRadius * 0.12f) {
            const float x = d.x / length;
            const float y = d.y / length;
            if (x < -0.38f) held |= m_left;
            if (x > 0.38f) held |= m_right;
            if (y < -0.38f) held |= m_up;
            if (y > 0.38f) held |= m_down;
            continue;
        }
        for (const TouchButton& b : m_buttons) {
            if (!b.arrow && inside(b.area, t, m_slack)) held |= b.actions;
        }
    }
    return held;
}

} // namespace px::runtime
