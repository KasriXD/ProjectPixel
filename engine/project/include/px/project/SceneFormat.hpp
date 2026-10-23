#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/core/FileSystem.hpp"
#include "px/core/Result.hpp"
#include "px/project/Project.hpp"
#include "px/scene/Scene.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

// Scene files ("*.scene.json"). Format reference: docs/project-format.md.
namespace px::project {

inline constexpr int kSceneFormatVersion = 3;

inline constexpr std::size_t kMaxEntitiesPerScene = 100000;

// Parses a scene. When `assets` is given, texture and script references are
// checked against it so broken references are reported at load time.
// Older format versions are migrated; migration notes go to `warnings` if
// given, otherwise to the log.
[[nodiscard]] Result<scene::Scene> parseScene(std::string_view json, std::string_view sourceName,
                                              const assets::AssetRegistry* assets = nullptr,
                                              std::vector<std::string>* warnings = nullptr);

// Loads the scene asset `sceneId` of `project` from `fs`.
[[nodiscard]] Result<scene::Scene> loadScene(const IFileSystem& fs, const ProjectManifest& project,
                                             assets::AssetId sceneId);

// Writes all authored (non-runtime) data. Output is deterministic.
[[nodiscard]] std::string serializeScene(const scene::Scene& scene);

// Serializes `entity` and its descendants as a standalone "px.entities"
// document (used for copy and paste), and reads one back into `scene` under
// `parent`, with new ids. References between the pasted entities are kept;
// references to entities outside them are kept only if those exist in
// `scene`. Returns the new top-level entities.
[[nodiscard]] std::string serializeEntities(const scene::Scene& scene, std::span<const scene::Entity> roots);
[[nodiscard]] Result<std::vector<scene::Entity>> pasteEntities(scene::Scene& scene, std::string_view json,
                                                               const assets::AssetRegistry* assets,
                                                               scene::Entity parent = scene::kNullEntity);

} // namespace px::project
