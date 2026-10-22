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

KeyBindings::KeyBindings(const project::InputConfig& config, const ActionSet& actionSet) {
    for (const auto& action : config.actions) {
        const ActionId id = actionSet.find(action.name);
        if (id == kInvalidAction) continue;
        for (const Key key : action.keys) {
            const auto index = static_cast<std::size_t>(key);
            if (index < kKeyCount) m_keyToActions[index] |= (1u << id);
        }
    }
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
