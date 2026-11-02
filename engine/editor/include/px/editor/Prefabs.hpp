#pragma once

#include "px/assets/AssetId.hpp"
#include "px/core/Math.hpp"
#include "px/core/Result.hpp"
#include "px/scene/Scene.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// Studio's prefab workflow, without UI (docs/prefabs.md): saving entities
// as a prefab, placing instances, applying an instance's changes to its
// prefab and every other instance, and reverting an instance.
namespace px::editor {

class EditorProject;
class SceneDocument;

struct PrefabUpdateReport {
    std::size_t instancesUpdated = 0;       // other instances, in the open scene and in files
    std::vector<std::string> filesUpdated;  // names of the scenes and prefabs rewritten on disk
    std::vector<std::string> warnings;
};

// "Save as Prefab": creates a prefab from `entity` and its descendants and
// makes `entity` an instance of it (one undo step). Fails for an entity
// inside a prefab instance (apply to that prefab instead).
[[nodiscard]] Result<assets::AssetId> createPrefabFromEntity(EditorProject& project, SceneDocument& doc,
                                                             scene::Entity entity, std::string_view name);

// Places an instance of the prefab with its root at world `position` under
// `parent`, and selects it (one undo step).
[[nodiscard]] Result<scene::Entity> placePrefab(EditorProject& project, SceneDocument& doc, assets::AssetId prefab,
                                                Vec2 position, scene::Entity parent = scene::kNullEntity);

// "Apply to Prefab": saves the instance at `root` as its prefab's new content
// and updates every other instance, keeping their own changes: in the open
// scene (one undo step) and in the project's other scenes and prefabs
// (rewritten at once, which cannot be undone).
[[nodiscard]] Result<PrefabUpdateReport> applyToPrefab(EditorProject& project, SceneDocument& doc, scene::Entity root);

// "Revert to Prefab": makes the instance a fresh copy of its prefab; only
// its position, name and place in the hierarchy stay (one undo step).
[[nodiscard]] Result<scene::Entity> revertToPrefab(EditorProject& project, SceneDocument& doc, scene::Entity root);

// The instance root `entity` belongs to (itself, or its nearest ancestor with
// a PrefabInstance component), or kNullEntity.
[[nodiscard]] scene::Entity instanceRootOf(const scene::Scene& scene, scene::Entity entity);

} // namespace px::editor
