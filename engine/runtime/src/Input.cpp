#include "px/runtime/Input.hpp"

namespace px::runtime {

ActionSet::ActionSet(const project::InputConfig& config) {
    for (const auto& action : config.actions) {
        if (m_names.size() >= kMaxActions) break; // the project loader enforces this limit
        m_names.push_back(action.name);
    }
}

ActionId ActionSet::find(std::string_view name) const {
    for (std::size_t i = 0; i < m_names.size(); ++i) {
        if (m_names[i] == name) return static_cast<ActionId>(i);
    }
    return kInvalidAction;
}

std::string_view ActionSet::name(ActionId id) const {
    return id < m_names.size() ? std::string_view(m_names[id]) : std::string_view();
}

std::optional<PadButton> padButtonForKey(Key key) noexcept {
    switch (key) {
        case Key::Left:
        case Key::A: return PadButton::DPadLeft;
        case Key::Right:
        case Key::D: return PadButton::DPadRight;
        case Key::Up:
        case Key::W: return PadButton::DPadUp;
        case Key::Down:
        case Key::S: return PadButton::DPadDown;
        case Key::Space:
        case Key::Z:
        case Key::J: return PadButton::South;
        case Key::X:
        case Key::K: return PadButton::West;
        case Key::C:
        case Key::E:
        case Key::L: return PadButton::East;
        case Key::V:
        case Key::Q:
        case Key::I: return PadButton::North;
        case Key::Enter: return PadButton::Start;
        case Key::Tab:
        case Key::Backspace: return PadButton::Back;
        case Key::LeftShift:
        case Key::RightShift: return PadButton::LeftShoulder;
        case Key::LeftCtrl:
        case Key::RightCtrl: return PadButton::RightShoulder;
        default: return std::nullopt;
    }
}

KeyBindings::KeyBindings(const project::InputConfig& config, const ActionSet& actionSet) {
    for (const auto& action : config.actions) {
        const ActionId id = actionSet.find(action.name);
        if (id == kInvalidAction) continue;
        for (const Key key : action.keys) {
            const auto index = static_cast<std::size_t>(key);
            if (index < kKeyCount) m_keyToActions[index] |= (1u << id);
            if (const auto pad = padButtonForKey(key)) m_padToActions[static_cast<std::size_t>(*pad)] |= (1u << id);
        }
    }
}

std::uint32_t KeyBindings::heldActions(const PadStates& pad) const {
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < kPadButtonCount; ++i) {
        if (pad[i]) mask |= m_padToActions[i];
    }
    return mask;
}

std::uint32_t KeyBindings::actionsForPad(PadButton button) const {
    const auto index = static_cast<std::size_t>(button);
    return index < kPadButtonCount ? m_padToActions[index] : 0u;
}

std::uint32_t KeyBindings::heldActions(const KeyStates& keys) const {
    std::uint32_t mask = 0;
    for (std::size_t i = 0; i < kKeyCount; ++i) {
        if (keys[i]) mask |= m_keyToActions[i];
    }
    return mask;
}

std::uint32_t KeyBindings::actionsForKey(Key key) const {
    const auto index = static_cast<std::size_t>(key);
    return index < kKeyCount ? m_keyToActions[index] : 0u;
}

} // namespace px::runtime
