#pragma once

// Internal: the entity and component readers and writers of the scene
// format (SceneFormat.cpp), shared with the prefab format (Prefab.cpp).

#include "JsonUtil.hpp"

#include "px/assets/AssetRegistry.hpp"
#include "px/scene/ComponentInfo.hpp"
#include "px/scene/Scene.hpp"

#include <string>
#include <vector>

namespace px::project::detail {

// Reads an "entities" array into `scene` and validates every entity.
void readEntityArray(scene::Scene& scene, const Json& entities, const assets::AssetRegistry* assets);
[[nodiscard]] Json writeEntityJson(const scene::Scene& scene, scene::Entity entity, bool writeParent);

// Upgrades an "entities" array written by scene format `sceneVersion` to
// the current scene format.
void migrateEntities(Json& entities, int sceneVersion, std::vector<std::string>& warnings);

// One component or field as it is written in files (authored fields only).
[[nodiscard]] Json componentToJson(const scene::ComponentInfo& component, const void* data);
// Adds the component to the entity (replacing it) and reads its fields.
void componentFromJson(scene::Scene& scene, scene::Entity entity, const scene::ComponentInfo& component,
                       const Json& json, const std::string& path);
[[nodiscard]] Json fieldToJson(const scene::FieldInfo& field, const void* data);
void fieldFromJson(const scene::FieldInfo& field, void* data, const Json& json, const std::string& path);

} // namespace px::project::detail
