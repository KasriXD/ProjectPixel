#include "px/scene/Registry.hpp"

#include "px/core/Log.hpp"

#include <cstdlib>
#include <format>

namespace px::scene {

namespace detail {

void missingComponent(Entity entity) noexcept {
    log::write(log::Level::Error, std::format("Fatal: entity {}:{} does not have the requested component",
                                              entity.index, entity.generation));
    std::abort();
}

} // namespace detail

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

void Registry::copyComponents(const Registry& source, Entity from, Entity to) {
    if (!source.isAlive(from) || !isAlive(to)) return;
    if (&source == this && from == to) return;
    // Component type ids are per process, so pool i holds the same type in
    // every registry.
    // (When source == this, every pool already exists, so m_pools is not
    // resized while `sourcePool` refers into it. ComponentPool::set takes its
    // value by copy, so copying within one pool is safe too.)
    for (std::size_t id = 0; id < source.m_pools.size(); ++id) {
        const detail::IComponentPool* sourcePool = source.m_pools[id].get();
        if (sourcePool == nullptr || !sourcePool->contains(from.index)) continue;
        if (id >= m_pools.size()) m_pools.resize(id + 1);
        if (!m_pools[id]) m_pools[id] = sourcePool->createEmpty();
        m_pools[id]->copyFrom(*sourcePool, from.index, to.index);
    }
}

void Registry::clearComponents(Entity entity) {
    if (!isAlive(entity)) return;
    for (auto& p : m_pools) {
        if (p) p->remove(entity.index);
    }
}

bool Registry::isAlive(Entity entity) const {
    return entity.index < m_generations.size() && m_alive[entity.index] != 0 &&
           m_generations[entity.index] == entity.generation;
}

} // namespace px::scene
