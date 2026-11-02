#include "px/editor/Prefabs.hpp"

#include "px/core/Log.hpp"
#include "px/editor/EditorProject.hpp"
#include "px/editor/SceneDocument.hpp"
#include "px/project/Prefab.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/scene/ComponentInfo.hpp"

#include <algorithm>
#include <format>

namespace px::editor {

using namespace px::scene;

namespace {

const Guid& idOf(const Scene& scene, Entity e) { return scene.get<EntityInfo>(e).id; }

const PrefabInstance* linkOf(const Scene& scene, Entity e) { return scene.tryGet<PrefabInstance>(e); }

// Updates every instance of `prefabId` in `scene` (except the one with id
// `skip`); returns how many were updated.
std::size_t updateAll(Scene& scene, assets::AssetId prefabId, const Scene& previous, const Scene& updated,
                      const Guid& skip, std::string_view where, std::vector<std::string>& warnings) {
    std::vector<Guid> ids;
    for (const Entity e : project::findInstances(scene, prefabId)) {
        if (idOf(scene, e) != skip) ids.push_back(idOf(scene, e));
    }
    std::size_t count = 0;
    for (const Guid& id : ids) {
        const Entity e = scene.findById(id);
        if (e.isNull()) continue; // was inside an instance updated before
        auto result = project::updateInstance(scene, e, previous, updated, true);
        if (!result) {
            warnings.push_back(std::format("{}: {}", where, result.error().message));
            continue;
        }
        ++count;
        for (const auto& w : result.value().warnings) warnings.push_back(std::format("{}: {}", where, w));
    }
    return count;
}

} // namespace

Entity instanceRootOf(const Scene& scene, Entity entity) {
    for (Entity e = entity; !e.isNull() && scene.isAlive(e); e = scene.parent(e)) {
        if (linkOf(scene, e) != nullptr) return e;
    }
    return kNullEntity;
}

Result<assets::AssetId> createPrefabFromEntity(EditorProject& project, SceneDocument& doc, Entity entity,
                                               std::string_view name) {
    const Scene& scene = doc.scene();
    if (!scene.isAlive(entity)) return makeError(ErrorCode::NotFound, "The entity does not exist");
    if (scene.subtree(entity).size() > project::kMaxEntitiesPerPrefab) {
        return makeError(ErrorCode::LimitExceeded, "A prefab can hold at most {} entities",
                         project::kMaxEntitiesPerPrefab);
    }
    std::size_t cleared = 0;
    const Scene content = project::prefabFromEntities(scene, entity, Guid::generate(), &cleared);
    auto id = project.createPrefab(name, content);
    if (!id) return id.error();
    doc.beginEdit("Save as Prefab");
    doc.sceneForEdit().set<PrefabInstance>(entity, PrefabInstance{id.value()});
    doc.markChanged();
    doc.endEdit();
    if (cleared > 0) {
        log::warn("Prefab '{}': {} reference(s) to entities outside it were left empty in the prefab",
                  project.asset(id.value())->name, cleared);
    }
    return id;
}

Result<Entity> placePrefab(EditorProject& project, SceneDocument& doc, assets::AssetId prefabId, Vec2 position,
                           Entity parent) {
    auto prefab = project.loadPrefab(prefabId);
    if (!prefab) return prefab.error();
    if (doc.scene().entityCount() + prefab.value().scene.entityCount() > project::kMaxEntitiesPerScene) {
        return makeError(ErrorCode::LimitExceeded, "The scene would have more than {} entities",
                         project::kMaxEntitiesPerScene);
    }
    doc.beginEdit("Place Prefab");
    auto created = project::instantiatePrefab(doc.sceneForEdit(), prefab.value().scene, prefabId, Guid::generate(),
                                              parent);
    if (!created) {
        doc.cancelEdit();
        return created.error();
    }
    Scene& scene = doc.sceneForEdit();
    const Entity root = created.value();
    auto& info = scene.get<EntityInfo>(root);
    if (!scene.findByName(info.name).isNull() && scene.findByName(info.name) != root) {
        info.name = doc.uniqueName(info.name);
    }
    scene.setWorldPosition(root, position);
    doc.followIfCameraIdle(root);
    doc.select(root);
    doc.markChanged();
    doc.endEdit();
    return root;
}

Result<PrefabUpdateReport> applyToPrefab(EditorProject& project, SceneDocument& doc, Entity root) {
    const Scene& scene = doc.scene();
    const PrefabInstance* link = scene.isAlive(root) ? linkOf(scene, root) : nullptr;
    if (link == nullptr) return makeError(ErrorCode::InvalidArgument, "The entity is not a prefab instance");
    const assets::AssetId prefabId = link->prefab;
    auto previous = project.loadPrefab(prefabId);
    if (!previous) return previous.error();
    const std::string prefabName = previous.value().name;
    for (const Entity e : scene.subtree(root)) {
        const PrefabInstance* inner = linkOf(scene, e);
        if (e != root && inner != nullptr && inner->prefab == prefabId) {
            return makeError(ErrorCode::InvalidArgument, "'{}' contains another instance of '{}'; a prefab cannot "
                                                         "contain itself",
                             scene.get<EntityInfo>(root).name, prefabName);
        }
    }
    if (scene.subtree(root).size() > project::kMaxEntitiesPerPrefab) {
        return makeError(ErrorCode::LimitExceeded, "A prefab can hold at most {} entities",
                         project::kMaxEntitiesPerPrefab);
    }
    const Scene& before = previous.value().scene;
    std::size_t cleared = 0;
    const Scene content = project::prefabFromEntities(scene, root, idOf(before, before.entities().front()), &cleared);
    if (auto saved = project.savePrefab(prefabId, content); !saved) return saved.error();

    PrefabUpdateReport report;
    if (cleared > 0) {
        report.warnings.push_back(
            std::format("{} reference(s) to entities outside the prefab were left empty in it", cleared));
    }
    const Guid rootId = idOf(scene, root);
    doc.beginEdit("Apply to Prefab");
    const std::size_t here = updateAll(doc.sceneForEdit(), prefabId, before, content, rootId, "this scene", report.warnings);
    if (here > 0) doc.markChanged();
    doc.endEdit();
    report.instancesUpdated += here;

    for (const auto& entry : project.assets().entries()) {
        if (entry.id == doc.assetId() || entry.id == prefabId) continue;
        if (entry.type == assets::AssetType::Scene) {
            auto other = project.loadScene(entry.id);
            if (!other) {
                report.warnings.push_back(std::format("{}: not updated: {}", entry.name, other.error().message));
                continue;
            }
            const std::size_t n = updateAll(other.value(), prefabId, before, content, Guid{}, entry.name, report.warnings);
            if (n == 0) continue;
            if (auto saved = project.saveScene(entry.id, other.value()); !saved) {
                report.warnings.push_back(std::format("{}: not saved: {}", entry.name, saved.error().message));
                continue;
            }
            report.instancesUpdated += n;
            report.filesUpdated.push_back(entry.name);
        } else if (entry.type == assets::AssetType::Prefab) {
            auto other = project.loadPrefab(entry.id);
            if (!other) {
                report.warnings.push_back(std::format("{}: not updated: {}", entry.name, other.error().message));
                continue;
            }
            const std::size_t n =
                updateAll(other.value().scene, prefabId, before, content, Guid{}, entry.name, report.warnings);
            if (n == 0) continue;
            if (auto saved = project.savePrefab(entry.id, other.value().scene); !saved) {
                report.warnings.push_back(std::format("{}: not saved: {}", entry.name, saved.error().message));
                continue;
            }
            report.instancesUpdated += n;
            report.filesUpdated.push_back(entry.name);
        }
    }
    log::info("Applied '{}' to prefab '{}'; {} other instance(s) updated{}", scene.get<EntityInfo>(root).name,
              prefabName, report.instancesUpdated,
              report.filesUpdated.empty() ? std::string() : std::format(" (also saved: {})", [&] {
                  std::string names;
                  for (const auto& n : report.filesUpdated) names += (names.empty() ? "" : ", ") + n;
                  return names;
              }()));
    for (const auto& w : report.warnings) log::warn("Apply to prefab '{}': {}", prefabName, w);
    return report;
}

Result<Entity> revertToPrefab(EditorProject& project, SceneDocument& doc, Entity root) {
    const PrefabInstance* link = doc.scene().isAlive(root) ? linkOf(doc.scene(), root) : nullptr;
    if (link == nullptr) return makeError(ErrorCode::InvalidArgument, "The entity is not a prefab instance");
    auto prefab = project.loadPrefab(link->prefab);
    if (!prefab) return prefab.error();
    doc.beginEdit("Revert to Prefab");
    auto result = project::updateInstance(doc.sceneForEdit(), root, prefab.value().scene, prefab.value().scene, false);
    if (!result) {
        doc.cancelEdit();
        return result.error();
    }
    doc.select(result.value().root);
    doc.markChanged();
    doc.endEdit();
    for (const auto& w : result.value().warnings) log::warn("Revert to prefab: {}", w);
    return result.value().root;
}

} // namespace px::editor
