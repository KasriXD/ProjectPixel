#pragma once

#include "px/core/Color.hpp"
#include "px/core/Guid.hpp"
#include "px/core/Result.hpp"
#include "px/scene/Components.hpp"
#include "px/scene/Registry.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::scene {

struct SceneSettings {
    Color backgroundColor{135, 196, 235, 255};
    // Players falling below this Y are respawned. Unset: no kill plane.
    std::optional<float> killPlaneY;
};

// A scene is a named set of entities plus scene-wide settings.
// Copyable: copies are independent (used for restart and future Studio play mode).
class Scene {
public:
    Scene() = default;

    [[nodiscard]] const std::string& name() const { return m_name; }
    void setName(std::string name) { m_name = std::move(name); }

    [[nodiscard]] SceneSettings& settings() { return m_settings; }
    [[nodiscard]] const SceneSettings& settings() const { return m_settings; }

    [[nodiscard]] Registry& registry() { return m_registry; }
    [[nodiscard]] const Registry& registry() const { return m_registry; }

    // Creates an entity with EntityInfo and a fresh random id.
    Entity createEntity(std::string name);
    // Creates an entity with a specific id; fails if the id is nil or in use.
    [[nodiscard]] Result<Entity> createEntityWithId(Guid id, std::string name);

    void destroyEntity(Entity entity);

    [[nodiscard]] bool isAlive(Entity entity) const { return m_registry.isAlive(entity); }
    [[nodiscard]] Entity findById(Guid id) const;
    // First entity with this name in creation order, or kNullEntity.
    [[nodiscard]] Entity findByName(std::string_view name) const;

    // Live entities in creation order (stable; used for saving and tools).
    [[nodiscard]] const std::vector<Entity>& entities() const { return m_order; }
    [[nodiscard]] std::size_t entityCount() const { return m_order.size(); }

    // Convenience accessors.
    template <class T>
    T& set(Entity e, T value = T{}) { return m_registry.set<T>(e, std::move(value)); }
    template <class T>
    [[nodiscard]] T* tryGet(Entity e) { return m_registry.tryGet<T>(e); }
    template <class T>
    [[nodiscard]] const T* tryGet(Entity e) const { return m_registry.tryGet<T>(e); }

private:
    std::string m_name;
    SceneSettings m_settings;
    Registry m_registry;
    std::unordered_map<Guid, Entity> m_byId;
    std::vector<Entity> m_order;
};

} // namespace px::scene
