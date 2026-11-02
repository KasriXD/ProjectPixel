#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/core/FileSystem.hpp"
#include "px/core/Guid.hpp"
#include "px/core/Result.hpp"
#include "px/project/Project.hpp"
#include "px/scene/Scene.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Prefabs: trees of entities saved as assets ("*.prefab.json"), placed in
// scenes as instances and spawned by scripts (World.spawn). Format and
// behaviour: docs/prefabs.md.
//
// Ids. An instance's entities get the prefab's entity ids XOR an instance
// key (instance root id XOR prefab root id). So the root keeps any id it is
// given, every other entity's id follows from it, the prefab entity an
// instance entity came from is found without stored links, and copying an
// instance (paste, duplicate) only needs to re-key it. Nested instances
// stay consistent because XOR composes.
namespace px::project {

inline constexpr int kPrefabFormatVersion = 1;
inline constexpr std::size_t kMaxEntitiesPerPrefab = 10000;

// A prefab's content: `scene` holds exactly one root entity (the first),
// without a PrefabInstance component of its own.
struct Prefab {
    assets::AssetId id;
    std::string name;
    scene::Scene scene;

    [[nodiscard]] scene::Entity root() const;
};

// Parses a prefab file. With `assets`, asset references are checked.
[[nodiscard]] Result<scene::Scene> parsePrefab(std::string_view json, std::string_view sourceName,
                                               const assets::AssetRegistry* assets = nullptr,
                                               std::vector<std::string>* warnings = nullptr);
// Deterministic text of a prefab (its scene must have exactly one root).
[[nodiscard]] std::string serializePrefab(const scene::Scene& content);
[[nodiscard]] Result<Prefab> loadPrefab(const IFileSystem& fs, const ProjectManifest& project, assets::AssetId id);

// All prefabs of a project, loaded once (immutable while a game runs).
class PrefabLibrary {
public:
    void add(Prefab prefab);
    [[nodiscard]] const Prefab* find(assets::AssetId id) const;
    [[nodiscard]] const Prefab* findByName(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> names() const;
    [[nodiscard]] std::size_t size() const { return m_prefabs.size(); }

private:
    std::vector<Prefab> m_prefabs;
};

// Loads every prefab asset of the project. Fails on the first broken one.
[[nodiscard]] Result<std::shared_ptr<const PrefabLibrary>> loadPrefabLibrary(const IFileSystem& fs,
                                                                              const ProjectManifest& project,
                                                                              std::vector<std::string>* warnings = nullptr);

// --- Instances -------------------------------------------------------------------------------

// The key of an instance whose root has `instanceRoot` as id.
[[nodiscard]] Guid instanceKey(const Guid& instanceRoot, const Guid& prefabRoot);
// Id of the instance entity made from prefab entity `prefabEntity` (and back: XOR is its own inverse).
[[nodiscard]] Guid instanceEntityId(const Guid& key, const Guid& prefabEntity);

// Copies `prefab` into `scene` under `parent` (before `before`, see
// Scene::setParent) as an instance of `prefabId`: the root gets `rootId` and
// a PrefabInstance component, references between the prefab's entities
// point to the copies. Fails, changing nothing, if an id is taken.
[[nodiscard]] Result<scene::Entity> instantiatePrefab(scene::Scene& scene, const scene::Scene& prefab,
                                                      assets::AssetId prefabId, const Guid& rootId,
                                                      scene::Entity parent = scene::kNullEntity,
                                                      scene::Entity before = scene::kNullEntity);

// Copies `root` and its descendants from `source` into `target` under
// `parent` with new ids, keeping prefab instances consistent (instances are
// re-keyed instead of getting random ids). References between the copies
// point to the copies. Returns the copy of root, or kNullEntity.
scene::Entity copyEntities(scene::Scene& target, const scene::Scene& source, scene::Entity root,
                           scene::Entity parent = scene::kNullEntity, scene::Entity before = scene::kNullEntity);

// Prefab content made from an entity tree in a scene ("Save as Prefab",
// "Apply to Prefab"). The prefab root gets `prefabRootId` (a new prefab:
// any fresh id; applying: the prefab's existing root id), so the tree is an
// instance of the result as it stands. The root's position becomes [0, 0],
// its PrefabInstance component is dropped, and references to entities
// outside the tree are cleared (`clearedReferences` counts them).
[[nodiscard]] scene::Scene prefabFromEntities(const scene::Scene& scene, scene::Entity root, const Guid& prefabRootId,
                                              std::size_t* clearedReferences = nullptr);

struct InstanceUpdate {
    scene::Entity root;                 // the instance's root afterwards
    std::size_t keptChanges = 0;        // instance changes carried over (values, names, added/removed components)
    std::size_t keptEntities = 0;       // entities added to this instance that were kept
    std::vector<std::string> warnings;  // e.g. a kept change that no longer fits
};

// Brings the instance at `root` up to date with `updated`, the new content
// of its prefab, whose previous content was `previous`. With keepChanges,
// what was changed in this instance relative to `previous` (field values,
// names, script property values, added or removed components, added or
// deleted entities) is kept; without, the instance becomes a fresh copy of
// `updated` (only its position, name and place in the hierarchy stay).
// The root keeps its id; ids of entities that persist do not change.
[[nodiscard]] Result<InstanceUpdate> updateInstance(scene::Scene& scene, scene::Entity root,
                                                    const scene::Scene& previous, const scene::Scene& updated,
                                                    bool keepChanges = true);

// Roots of the instances of `prefab` in the scene, in hierarchy order.
[[nodiscard]] std::vector<scene::Entity> findInstances(const scene::Scene& scene, assets::AssetId prefab);

} // namespace px::project
