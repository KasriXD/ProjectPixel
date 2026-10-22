#include "px/scene/Registry.hpp"

namespace px::scene {

Registry::Registry(const Registry& other)
    : m_generations(other.m_generations),
      m_alive(other.m_alive),
      m_freeList(other.m_freeList),
      m_aliveCount(other.m_aliveCount) {
    m_pools.reserve(other.m_pools.size());
    for (const auto& p : other.m_pools) {
        m_pools.push_back(p ? p->clone() : nullptr);
    }
}

Registry& Registry::operator=(const Registry& other) {
    if (this != &other) {
        Registry copy(other);
        *this = std::move(copy);
    }
    return *this;
}

Entity Registry::create() {
    std::uint32_t index;
    if (!m_freeList.empty()) {
        index = m_freeList.back();
        m_freeList.pop_back();
    } else {
        index = static_cast<std::uint32_t>(m_generations.size());
        m_generations.push_back(0);
        m_alive.push_back(0);
    }
    m_alive[index] = 1;
    ++m_aliveCount;
    return Entity{index, m_generations[index]};
}

void Registry::destroy(Entity entity) {
    if (!isAlive(entity)) return;
    for (auto& p : m_pools) {
        if (p) p->remove(entity.index);
    }
    m_alive[entity.index] = 0;
    ++m_generations[entity.index];
    m_freeList.push_back(entity.index);
    --m_aliveCount;
}

bool Registry::isAlive(Entity entity) const {
    return entity.index < m_generations.size() && m_alive[entity.index] != 0 &&
           m_generations[entity.index] == entity.generation;
}

} // namespace px::scene
