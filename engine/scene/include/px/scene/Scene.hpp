#pragma once

#include "px/core/Color.hpp"
#include "px/core/Guid.hpp"
#include "px/core/Result.hpp"
#include "px/scene/Components.hpp"
#include "px/scene/Registry.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace px::scene {

struct SceneSettings {
    Color backgroundColor{135, 196, 235, 255};
    // Players falling below this Y are respawned. Unset: no kill plane.
    std::optional<float> killPlaneY;
};

// Deepest allowed parent/child nesting (root = depth 0).
inline constexpr int kMaxHierarchyDepth = 64;

// A scene is a named set of entities plus scene-wide settings.
// Copyable: copies are independent (used for play mode, restart and undo).
//
// Hierarchy: every entity has at most one parent. entities() lists all
// entities in hierarchy order: a depth-first, pre-order walk where each
// entity comes right after its parent's earlier children and their
// descendants. So every subtree is a contiguous range, files are written in
// this order, and it is also the draw order within a layer and the order
// scripts run in. A Transform is relative to the parent (see worldTransform).
class Scene {
public:
    Scene() = default;

    [[nodiscard]] const std::string& name() const { return m_name; }
    void setName(std::string name) { m_name = std::move(name); }

    [[nodiscard]] SceneSettings& settings() { return m_settings; }
    [[nodiscard]] const SceneSettings& settings() const { return m_settings; }

    [[nodiscard]] Registry& registry() { return m_registry; }
    [[nodiscard]] const Registry& registry() const { return m_registry; }

    // Creates an entity with EntityInfo and a fresh random id, as the last
    // child of `parent` (or as the last root entity).
    Entity createEntity(std::string name, Entity parent = kNullEntity);
    // Creates an entity with a specific id; fails if the id is nil or in use.
    [[nodiscard]] Result<Entity> createEntityWithId(Guid id, std::string name, Entity parent = kNullEntity);

    // Destroys the entity and all of its descendants.
    void destroyEntity(Entity entity);

    [[nodiscard]] bool isAlive(Entity entity) const { return m_registry.isAlive(entity); }
    [[nodiscard]] Entity findById(Guid id) const;
    // First entity with this name in hierarchy order, or kNullEntity.
    [[nodiscard]] Entity findByName(std::string_view name) const;

    // Live entities in hierarchy order (see class comment).
    [[nodiscard]] const std::vector<Entity>& entities() const { return m_order; }
    [[nodiscard]] std::size_t entityCount() const { return m_order.size(); }

    // --- Hierarchy ---------------------------------------------------------------------
    [[nodiscard]] Entity parent(Entity entity) const;
    [[nodiscard]] std::vector<Entity> children(Entity entity) const;
    [[nodiscard]] bool hasChildren(Entity entity) const;
    // True if `ancestor` is the parent, grandparent, ... of `entity`.
    [[nodiscard]] bool isAncestor(Entity ancestor, Entity entity) const;
    // 0 for root entities.
    [[nodiscard]] int depth(Entity entity) const;
    // The entity followed by all of its descendants, in hierarchy order.
    [[nodiscard]] std::vector<Entity> subtree(Entity entity) const;
    // Position in entities(), or npos for dead entities.
    [[nodiscard]] std::size_t orderIndex(Entity entity) const;
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    // Moves `entity` (with its descendants) under `newParent` (kNullEntity:
    // make it a root). It is placed before `before`, which must then be a
    // child of newParent (or a root), otherwise after the last child. With
    // keepWorldTransform the entity stays where it is on screen; otherwise
    // its local Transform is kept. Fails on cycles and excessive depth.
    Status setParent(Entity entity, Entity newParent, Entity before = kNullEntity, bool keepWorldTransform = false);

    // Sets many parent links at once (pairs of child, parent), then restores
    // hierarchy order while keeping the existing relative order of
    // siblings. O(n); used when loading files. Fails without changes on
    // dead entities, cycles or excessive depth.
    Status setParents(std::span<const std::pair<Entity, Entity>> links);

    // --- Transforms ----------------------------------------------------------------------
    // The entity's transform in world space: its Transform composed with
    // those of its ancestors. Entities without a Transform contribute none.
    [[nodiscard]] Transform worldTransform(Entity entity) const;
    // World transform of the entity's parent (identity for roots).
    [[nodiscard]] Transform parentWorldTransform(Entity entity) const;
    [[nodiscard]] Vec2 worldPosition(Entity entity) const { return worldTransform(entity).position; }
    // Set the world position/rotation by changing the local Transform.
    // No-op for entities without a Transform.
    void setWorldPosition(Entity entity, Vec2 position);
    void setWorldRotation(Entity entity, float degrees);

    // --- Copying -------------------------------------------------------------------------
    // Copies `root` and its descendants from `source` (which may be this
    // scene) under `parent`, before `before` (see setParent). Copies get new
    // ids; names, components and the subtree's internal structure are kept.
    // Returns the copy of root, or kNullEntity if root is not alive in source.
    Entity copySubtree(const Scene& source, Entity root, Entity parent = kNullEntity, Entity before = kNullEntity);

    // Convenience accessors.
    template <class T>
    T& set(Entity e, T value = T{}) { return m_registry.set<T>(e, std::move(value)); }
    template <class T>
    [[nodiscard]] T* tryGet(Entity e) { return m_registry.tryGet<T>(e); }
    template <class T>
    [[nodiscard]] const T* tryGet(Entity e) const { return m_registry.tryGet<T>(e); }

private:
    [[nodiscard]] std::size_t subtreeEnd(std::size_t index) const; // one past the last descendant
    [[nodiscard]] int subtreeHeight(Entity entity) const;
    void setParentLink(Entity child, Entity parent);

    std::string m_name;
    SceneSettings m_settings;
    Registry m_registry;
    std::unordered_map<Guid, Entity> m_byId;
    std::vector<Entity> m_order;
    std::vector<Entity> m_parents; // indexed by entity index
};

} // namespace px::scene
