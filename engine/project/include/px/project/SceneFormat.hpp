#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/core/FileSystem.hpp"
#include "px/core/Result.hpp"
#include "px/project/Project.hpp"
#include "px/scene/Scene.hpp"

#include <string>
#include <string_view>

// Scene files ("*.scene.json"). Format reference: docs/project-format.md.
namespace px::project {

inline constexpr int kSceneFormatVersion = 1;

inline constexpr std::size_t kMaxEntitiesPerScene = 100000;
inline constexpr int kMaxTilemapDimension = 4096;
inline constexpr std::size_t kMaxAnimationFrames = 1024;

// Parses a scene. When `assets` is given, texture references are checked
// against it so broken references are reported at load time.
[[nodiscard]] Result<scene::Scene> parseScene(std::string_view json, std::string_view sourceName,
                                              const assets::AssetRegistry* assets = nullptr);

// Loads the scene asset `sceneId` of `project` from `fs`.
[[nodiscard]] Result<scene::Scene> loadScene(const IFileSystem& fs, const ProjectManifest& project,
                                             assets::AssetId sceneId);

// Writes all authored (non-runtime) data. Output is deterministic.
[[nodiscard]] std::string serializeScene(const scene::Scene& scene);

} // namespace px::project
