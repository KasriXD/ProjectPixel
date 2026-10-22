#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

// Entity/component storage. Entities are generational handles; each component
// type is stored densely in its own sparse-set pool. Components are plain data
// structs; behaviour lives in systems (runtime module).
namespace px::scene {

struct Entity {
    static constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFu;

    std::uint32_t index = kInvalidIndex;
    std::uint32_t generation = 0;

    [[nodiscard]] constexpr bool isNull() const { return index == kInvalidIndex; }
    constexpr bool operator==(const Entity&) const = default;
};

inline constexpr Entity kNullEntity{};

namespace detail {

inline std::uint32_t nextComponentTypeId() noexcept {
    static std::atomic<std::uint32_t> counter{0};
    return counter.fetch_add(1);
}

// Dense per-process id for each component type (static linking only).
template <class T>
std::uint32_t componentTypeId() noexcept {
    static const std::uint32_t id = nextComponentTypeId();
    return id;
}

class IComponentPool {
public:
    virtual ~IComponentPool() = default;
    virtual void remove(std::uint32_t entityIndex) = 0;
    [[nodiscard]] virtual bool contains(std::uint32_t entityIndex) const = 0;
    [[nodiscard]] virtual std::unique_ptr<IComponentPool> clone() const = 0;
};

template <class T>
class ComponentPool final : public IComponentPool {
public:
    static constexpr std::uint32_t npos = 0xFFFFFFFFu;

    T& set(std::uint32_t entityIndex, T value) {
        if (entityIndex >= m_sparse.size()) m_sparse.resize(entityIndex + 1, npos);
        std::uint32_t& slot = m_sparse[entityIndex];
        if (slot != npos) {
            m_data[slot] = std::move(value);
            return m_data[slot];
        }
        slot = static_cast<std::uint32_t>(m_dense.size());
        m_dense.push_back(entityIndex);
        m_data.push_back(std::move(value));
        return m_data.back();
    }

    [[nodiscard]] T* get(std::uint32_t entityIndex) {
        if (entityIndex >= m_sparse.size() || m_sparse[entityIndex] == npos) return nullptr;
        return &m_data[m_sparse[entityIndex]];
    }
    [[nodiscard]] const T* get(std::uint32_t entityIndex) const {
        if (entityIndex >= m_sparse.size() || m_sparse[entityIndex] == npos) return nullptr;
        return &m_data[m_sparse[entityIndex]];
    }

    // Precondition: contains(entityIndex).
    [[nodiscard]] T& at(std::uint32_t entityIndex) { return m_data[m_sparse[entityIndex]]; }

    void remove(std::uint32_t entityIndex) override {
        if (entityIndex >= m_sparse.size() || m_sparse[entityIndex] == npos) return;
        const std::uint32_t slot = m_sparse[entityIndex];
        const std::uint32_t last = static_cast<std::uint32_t>(m_dense.size() - 1);
        if (slot != last) {
            m_dense[slot] = m_dense[last];
            m_data[slot] = std::move(m_data[last]);
            m_sparse[m_dense[slot]] = slot;
        }
        m_dense.pop_back();
        m_data.pop_back();
        m_sparse[entityIndex] = npos;
    }

    [[nodiscard]] bool contains(std::uint32_t entityIndex) const override {
        return entityIndex < m_sparse.size() && m_sparse[entityIndex] != npos;
    }

    [[nodiscard]] std::unique_ptr<IComponentPool> clone() const override {
        return std::make_unique<ComponentPool<T>>(*this);
    }

    [[nodiscard]] std::size_t size() const { return m_dense.size(); }
    [[nodiscard]] std::uint32_t entityIndexAt(std::size_t denseIndex) const { return m_dense[denseIndex]; }
    [[nodiscard]] T& dataAt(std::size_t denseIndex) { return m_data[denseIndex]; }

private:
    std::vector<std::uint32_t> m_sparse;
    std::vector<std::uint32_t> m_dense;
    std::vector<T> m_data;
};

} // namespace detail

class Registry {
public:
    Registry() = default;
    Registry(const Registry& other);
    Registry& operator=(const Registry& other);
    Registry(Registry&&) noexcept = default;
    Registry& operator=(Registry&&) noexcept = default;
    ~Registry() = default;

    [[nodiscard]] Entity create();
    // Removes all components and invalidates the handle. No-op for dead handles.
    void destroy(Entity entity);
    [[nodiscard]] bool isAlive(Entity entity) const;
    [[nodiscard]] std::size_t aliveCount() const { return m_aliveCount; }

    // Adds or replaces component T.
    template <class T>
    T& set(Entity entity, T value = T{}) {
        assert(isAlive(entity));
        return ensurePool<T>().set(entity.index, std::move(value));
    }

    template <class T>
    [[nodiscard]] T* tryGet(Entity entity) {
        if (!isAlive(entity)) return nullptr;
        auto* p = pool<T>();
        return p ? p->get(entity.index) : nullptr;
    }
    template <class T>
    [[nodiscard]] const T* tryGet(Entity entity) const {
        if (!isAlive(entity)) return nullptr;
        const auto* p = pool<T>();
        return p ? p->get(entity.index) : nullptr;
    }

    template <class T>
    [[nodiscard]] bool has(Entity entity) const {
        return tryGet<T>(entity) != nullptr;
    }

    template <class T>
    void remove(Entity entity) {
        if (!isAlive(entity)) return;
        if (auto* p = pool<T>()) p->remove(entity.index);
    }

    template <class T>
    [[nodiscard]] std::size_t count() const {
        const auto* p = pool<T>();
        return p ? p->size() : 0;
    }

    // Calls fn(Entity, T&, Rest&...) for every entity that has all listed
    // components. Iterates backwards over T's pool, so fn may destroy the
    // current entity or remove its components. fn must not add components
    // of the iterated types to other entities.
    template <class T, class... Rest, class Fn>
    void each(Fn&& fn) {
        detail::ComponentPool<T>* const primary = pool<T>();
        if (primary == nullptr) return;
        // Pools are looked up once; they are heap objects, so the pointers stay
        // valid even if fn creates pools for other component types.
        const std::tuple<detail::ComponentPool<Rest>*...> others{pool<Rest>()...};
        if constexpr (sizeof...(Rest) > 0) {
            const bool missingPool = ((std::get<detail::ComponentPool<Rest>*>(others) == nullptr) || ...);
            if (missingPool) return;
        }
        for (std::size_t i = primary->size(); i-- > 0;) {
            if (i >= primary->size()) continue;
            const std::uint32_t index = primary->entityIndexAt(i);
            const Entity entity{index, m_generations[index]};
            if constexpr (sizeof...(Rest) == 0) {
                fn(entity, primary->dataAt(i));
            } else {
                if (!(std::get<detail::ComponentPool<Rest>*>(others)->contains(index) && ...)) continue;
                fn(entity, primary->dataAt(i), std::get<detail::ComponentPool<Rest>*>(others)->at(index)...);
            }
        }
    }

private:
    template <class T>
    detail::ComponentPool<T>* pool() {
        const auto id = detail::componentTypeId<T>();
        if (id >= m_pools.size() || !m_pools[id]) return nullptr;
        return static_cast<detail::ComponentPool<T>*>(m_pools[id].get());
    }
    template <class T>
    const detail::ComponentPool<T>* pool() const {
        const auto id = detail::componentTypeId<T>();
        if (id >= m_pools.size() || !m_pools[id]) return nullptr;
        return static_cast<const detail::ComponentPool<T>*>(m_pools[id].get());
    }
    template <class T>
    detail::ComponentPool<T>& ensurePool() {
        const auto id = detail::componentTypeId<T>();
        if (id >= m_pools.size()) m_pools.resize(id + 1);
        if (!m_pools[id]) m_pools[id] = std::make_unique<detail::ComponentPool<T>>();
        return *static_cast<detail::ComponentPool<T>*>(m_pools[id].get());
    }

    std::vector<std::uint32_t> m_generations;
    std::vector<std::uint8_t> m_alive;
    std::vector<std::uint32_t> m_freeList;
    std::vector<std::unique_ptr<detail::IComponentPool>> m_pools;
    std::size_t m_aliveCount = 0;
};

} // namespace px::scene
