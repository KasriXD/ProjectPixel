#include "px/project/Prefab.hpp"

#include "JsonUtil.hpp"
#include "SceneJson.hpp"

#include "px/core/Log.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/scene/ComponentInfo.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace px::project {

using detail::fail;
using detail::Json;
using detail::ObjectReader;
using namespace px::scene;

namespace {

const Guid& idOf(const Scene& scene, Entity e) { return scene.get<EntityInfo>(e).id; }

Guid xorIds(const Guid& a, const Guid& b) { return Guid(a.high() ^ b.high(), a.low() ^ b.low()); }

// Clears every reference in the scene to one of `ids`.
void clearReferences(Scene& scene, const std::unordered_set<Guid>& ids) {
    if (ids.empty()) return;
    for (const Entity e : scene.entities()) {
        forEachComponent(scene, e, [&](const ComponentInfo& c, void* data) {
            for (const FieldInfo& f : c.fields) {
                if (f.kind != FieldKind::EntityRef) continue;
                Guid& ref = f.ref<Guid>(data);
                if (ids.contains(ref)) ref = Guid{};
            }
        });
    }
}

bool isScripts(const ComponentInfo& c) { return !c.fields.empty() && c.fields.front().kind == FieldKind::ScriptList; }
bool isPrefabInstance(const ComponentInfo& c) { return c.name == "PrefabInstance"; }

// What an instance changed relative to the prefab it was made from.
struct ScriptPropertyChange {
    assets::AssetId script;
    int occurrence = 0;              // which attachment of that script (0: the first)
    std::string name;
    std::optional<ScriptValue> value; // nullopt: the instance does not set it (uses the default)
};

struct EntityChanges {
    std::optional<std::string> name;
    std::vector<std::pair<const ComponentInfo*, Json>> added;
    std::vector<const ComponentInfo*> removed;
    struct Field {
        const ComponentInfo* component;
        const FieldInfo* field;
        Json value;
    };
    std::vector<Field> fields;
    std::optional<Scripts> scripts; // the whole list, when scripts were added, removed or reordered
    std::vector<ScriptPropertyChange> properties;

    [[nodiscard]] std::size_t count() const {
        return (name ? 1 : 0) + added.size() + removed.size() + fields.size() + (scripts ? 1 : 0) + properties.size();
    }
};

int occurrenceOf(const Scripts& scripts, std::size_t index) {
    int n = 0;
    for (std::size_t i = 0; i < index; ++i) {
        if (scripts.items[i].script == scripts.items[index].script) ++n;
    }
    return n;
}

AttachedScript* findAttachment(Scripts& scripts, const assets::AssetId& script, int occurrence) {
    for (auto& item : scripts.items) {
        if (item.script != script) continue;
        if (occurrence-- == 0) return &item;
    }
    return nullptr;
}

void diffScripts(const Scripts& mine, const Scripts& theirs, EntityChanges& out) {
    const bool sameList = mine.items.size() == theirs.items.size() &&
                          std::equal(mine.items.begin(), mine.items.end(), theirs.items.begin(),
                                     [](const AttachedScript& a, const AttachedScript& b) { return a.script == b.script; });
    if (!sameList) {
        out.scripts = mine;
        return;
    }
    for (std::size_t i = 0; i < mine.items.size(); ++i) {
        const AttachedScript& a = mine.items[i];
        const AttachedScript& b = theirs.items[i];
        const int occurrence = occurrenceOf(mine, i);
        for (const ScriptProperty& p : a.properties) {
            const ScriptProperty* q = b.findProperty(p.name);
            if (q == nullptr || !(q->value == p.value)) out.properties.push_back({a.script, occurrence, p.name, p.value});
        }
        for (const ScriptProperty& q : b.properties) {
            if (a.findProperty(q.name) == nullptr) out.properties.push_back({a.script, occurrence, q.name, std::nullopt});
        }
    }
}

EntityChanges diffEntity(const Scene& scene, Entity mine, const Scene& prefab, Entity theirs, bool isRoot) {
    EntityChanges out;
    if (scene.get<EntityInfo>(mine).name != prefab.get<EntityInfo>(theirs).name) {
        out.name = scene.get<EntityInfo>(mine).name;
    }
    for (const ComponentInfo& c : componentTypes()) {
        if (isRoot && isPrefabInstance(c)) continue;
        const void* a = c.getConst(scene, mine);
        const void* b = c.getConst(prefab, theirs);
        if (a != nullptr && b == nullptr) {
            out.added.emplace_back(&c, detail::componentToJson(c, a));
        } else if (a == nullptr && b != nullptr) {
            out.removed.push_back(&c);
        } else if (a != nullptr && b != nullptr) {
            if (isScripts(c)) {
                diffScripts(*static_cast<const Scripts*>(a), *static_cast<const Scripts*>(b), out);
                continue;
            }
            for (const FieldInfo& f : c.fields) {
                if (f.isRuntime()) continue;
                Json value = detail::fieldToJson(f, a);
                if (value != detail::fieldToJson(f, b)) out.fields.push_back({&c, &f, std::move(value)});
            }
        }
    }
    return out;
}

// Applies one entity's changes; returns how many were applied.
std::size_t applyChanges(Scene& scene, Entity e, const EntityChanges& ch, const std::string& label,
                         std::vector<std::string>& warnings) {
    std::size_t applied = 0;
    if (ch.name) {
        scene.get<EntityInfo>(e).name = *ch.name;
        ++applied;
    }
    for (const ComponentInfo* c : ch.removed) {
        if (!c->has(scene, e)) continue;
        if (!dependents(scene, e, *c).empty()) {
            warnings.push_back(std::format("{}: kept {} (the prefab's other components need it)", label, c->name));
            continue;
        }
        c->remove(scene, e);
        ++applied;
    }
    for (const auto& [c, json] : ch.added) {
        try {
            detail::componentFromJson(scene, e, *c, json, std::string(c->name));
            ++applied;
        } catch (const detail::FormatError& error) {
            warnings.push_back(std::format("{}: could not keep {}: {}", label, c->name, error.what()));
        }
    }
    for (const auto& change : ch.fields) {
        void* data = change.component->get(scene, e);
        if (data == nullptr) continue; // the prefab no longer has the component
        try {
            detail::fieldFromJson(*change.field, data, change.value, std::string(change.field->name));
            ++applied;
        } catch (const detail::FormatError& error) {
            warnings.push_back(std::format("{}: could not keep {}.{}: {}", label, change.component->name,
                                           change.field->name, error.what()));
        }
    }
    if (ch.scripts) {
        scene.set<Scripts>(e, *ch.scripts);
        ++applied;
    }
    if (!ch.properties.empty()) {
        if (Scripts* scripts = scene.tryGet<Scripts>(e)) {
            for (const ScriptPropertyChange& p : ch.properties) {
                AttachedScript* item = findAttachment(*scripts, p.script, p.occurrence);
                if (item == nullptr) continue;
                auto it = std::find_if(item->properties.begin(), item->properties.end(),
                                       [&](const ScriptProperty& q) { return q.name == p.name; });
                if (!p.value) {
                    if (it != item->properties.end()) item->properties.erase(it);
                } else if (it != item->properties.end()) {
                    it->value = *p.value;
                } else {
                    item->properties.push_back({p.name, *p.value});
                }
                ++applied;
            }
        }
    }
    return applied;
}

} // namespace

// --- Format ----------------------------------------------------------------------------------

Entity Prefab::root() const { return scene.entities().empty() ? kNullEntity : scene.entities().front(); }

Result<Scene> parsePrefab(std::string_view json, std::string_view sourceName, const assets::AssetRegistry* assets,
                          std::vector<std::string>* warnings) {
    try {
        Json root = detail::parseJsonText(json);
        if (!root.is_object()) fail("", "a prefab file must contain a JSON object");
        ObjectReader r(root, "");
        std::string format;
        r.required("format", format);
        if (format != "px.prefab") fail("format", "expected \"format\": \"px.prefab\" (is this a prefab file?)");
        int version = 0;
        r.required("formatVersion", version);
        if (version < 1) fail("formatVersion", std::format("{} is not a valid version", version));
        if (version > kPrefabFormatVersion) {
            throw detail::FormatError(std::format(
                "this prefab uses format version {}, but this engine supports up to version {}; update the engine",
                version, kPrefabFormatVersion));
        }
        int sceneVersion = 0;
        r.required("sceneFormatVersion", sceneVersion);
        if (sceneVersion < 1) fail("sceneFormatVersion", std::format("{} is not a valid version", sceneVersion));
        if (sceneVersion > kSceneFormatVersion) {
            throw detail::FormatError(std::format("this prefab uses scene format version {}, but this engine supports "
                                                  "up to version {}; update the engine",
                                                  sceneVersion, kSceneFormatVersion));
        }
        Json entities = r.require("entities");
        r.finish();
        if (!entities.is_array()) fail("entities", "expected an array of entities");
        if (entities.empty()) fail("entities", "a prefab needs at least one entity");
        if (entities.size() > kMaxEntitiesPerPrefab) {
            fail("entities",
                 std::format("too many entities ({}); the limit is {}", entities.size(), kMaxEntitiesPerPrefab));
        }
        std::vector<std::string> migrationWarnings;
        if (sceneVersion < kSceneFormatVersion) detail::migrateEntities(entities, sceneVersion, migrationWarnings);

        Scene scene;
        detail::readEntityArray(scene, entities, assets);
        if (scene.children(kNullEntity).size() != 1) {
            fail("entities", "a prefab has exactly one root entity; every other entity needs a \"parent\" inside it");
        }
        scene.registry().remove<PrefabInstance>(scene.entities().front());
        for (const auto& w : migrationWarnings) {
            if (warnings != nullptr) {
                warnings->push_back(std::format("{}: {}", sourceName, w));
            } else {
                log::warn("{}: {}", sourceName, w);
            }
        }
        return scene;
    } catch (const detail::FormatError& e) {
        return makeError(ErrorCode::InvalidData, "{}: {}", sourceName, e.what());
    }
}

std::string serializePrefab(const Scene& content) {
    Json root;
    root["format"] = "px.prefab";
    root["formatVersion"] = kPrefabFormatVersion;
    root["sceneFormatVersion"] = kSceneFormatVersion;
    Json entities = Json::array();
    for (const Entity e : content.entities()) entities.push_back(detail::writeEntityJson(content, e, true));
    root["entities"] = std::move(entities);
    return detail::dumpPretty(root);
}

Result<Prefab> loadPrefab(const IFileSystem& fs, const ProjectManifest& project, assets::AssetId id) {
    const auto* entry = project.assets.find(id);
    if (entry == nullptr) return makeError(ErrorCode::NotFound, "Prefab {} is not listed in the project", id.toString());
    if (entry->type != assets::AssetType::Prefab) {
        return makeError(ErrorCode::InvalidArgument, "Asset '{}' is not a prefab", entry->name);
    }
    auto text = fs.readText(entry->path);
    if (!text) return text.error();
    auto scene = parsePrefab(text.value(), fs.describe(entry->path), &project.assets);
    if (!scene) return scene.error();
    return Prefab{id, entry->name, std::move(scene.value())};
}

void PrefabLibrary::add(Prefab prefab) { m_prefabs.push_back(std::move(prefab)); }

const Prefab* PrefabLibrary::find(assets::AssetId id) const {
    for (const Prefab& p : m_prefabs) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

const Prefab* PrefabLibrary::findByName(std::string_view name) const {
    for (const Prefab& p : m_prefabs) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

std::vector<std::string> PrefabLibrary::names() const {
    std::vector<std::string> out;
    for (const Prefab& p : m_prefabs) out.push_back(p.name);
    return out;
}

Result<std::shared_ptr<const PrefabLibrary>> loadPrefabLibrary(const IFileSystem& fs, const ProjectManifest& project,
                                                               std::vector<std::string>* warnings) {
    auto library = std::make_shared<PrefabLibrary>();
    for (const auto& entry : project.assets.entries()) {
        if (entry.type != assets::AssetType::Prefab) continue;
        auto text = fs.readText(entry.path);
        if (!text) return text.error();
        auto scene = parsePrefab(text.value(), fs.describe(entry.path), &project.assets, warnings);
        if (!scene) return scene.error();
        library->add(Prefab{entry.id, entry.name, std::move(scene.value())});
    }
    return std::shared_ptr<const PrefabLibrary>(std::move(library));
}

// --- Instances -------------------------------------------------------------------------------

Guid instanceKey(const Guid& instanceRoot, const Guid& prefabRoot) { return xorIds(instanceRoot, prefabRoot); }
Guid instanceEntityId(const Guid& key, const Guid& prefabEntity) { return xorIds(key, prefabEntity); }

Result<Entity> instantiatePrefab(Scene& scene, const Scene& prefab, assets::AssetId prefabId, const Guid& rootId,
                                 Entity parent, Entity before) {
    if (prefab.entities().empty()) return makeError(ErrorCode::InvalidData, "The prefab is empty");
    if (rootId.isNil()) return makeError(ErrorCode::InvalidArgument, "An instance needs a root id");
    const Entity prefabRoot = prefab.entities().front();
    const Guid key = instanceKey(rootId, idOf(prefab, prefabRoot));
    const Entity copy =
        scene.copySubtree(prefab, prefabRoot, [&key](const Guid& g) { return instanceEntityId(key, g); }, parent);
    if (copy.isNull()) {
        return makeError(ErrorCode::InvalidArgument,
                         "Cannot place the prefab here: the hierarchy would be too deep, or its ids are in use");
    }
    if (!before.isNull()) (void)scene.setParent(copy, parent, before);
    std::vector<std::pair<Guid, Guid>> remap;
    for (const Entity e : prefab.entities()) remap.emplace_back(idOf(prefab, e), instanceEntityId(key, idOf(prefab, e)));
    const std::vector<Entity> copies = scene.subtree(copy);
    remapEntityReferences(scene, copies, remap);
    scene.set<PrefabInstance>(copy, PrefabInstance{prefabId});
    return copy;
}

Entity copyEntities(Scene& target, const Scene& source, Entity root, Entity parent, Entity before) {
    if (!source.isAlive(root)) return kNullEntity;
    const std::vector<Entity> nodes = source.subtree(root);
    std::unordered_map<Guid, Guid> newIds;
    std::unordered_map<std::uint32_t, std::optional<Guid>> delta; // by entity index: re-keying of the instance it is in
    newIds.reserve(nodes.size());
    for (const Entity e : nodes) {
        const Guid& id = idOf(source, e);
        std::optional<Guid> inherited;
        if (e != root) inherited = delta.at(source.parent(e).index);
        Guid fresh;
        if (inherited) {
            fresh = xorIds(id, *inherited);
        } else if (source.tryGet<PrefabInstance>(e) != nullptr) {
            fresh = Guid::generate();
            inherited = xorIds(id, fresh);
        } else {
            fresh = Guid::generate();
        }
        delta.emplace(e.index, inherited);
        newIds.emplace(id, fresh);
    }
    const Entity copy = target.copySubtree(source, root, [&newIds](const Guid& g) { return newIds.at(g); }, parent);
    if (copy.isNull()) return kNullEntity;
    if (!before.isNull()) (void)target.setParent(copy, parent, before);
    std::vector<std::pair<Guid, Guid>> remap(newIds.begin(), newIds.end());
    const std::vector<Entity> copies = target.subtree(copy);
    remapEntityReferences(target, copies, remap);
    return copy;
}

Scene prefabFromEntities(const Scene& scene, Entity root, const Guid& prefabRootId, std::size_t* clearedReferences) {
    Scene out;
    if (!scene.isAlive(root)) return out;
    const Guid key = instanceKey(idOf(scene, root), prefabRootId);
    const Entity copy = out.copySubtree(scene, root, [&key](const Guid& g) { return instanceEntityId(key, g); });
    if (copy.isNull()) return out;
    std::vector<std::pair<Guid, Guid>> remap;
    for (const Entity e : scene.subtree(root)) remap.emplace_back(idOf(scene, e), instanceEntityId(key, idOf(scene, e)));
    const std::vector<Entity> copies = out.subtree(copy);
    remapEntityReferences(out, copies, remap);
    std::size_t cleared = 0;
    for (const Entity e : copies) {
        forEachComponent(out, e, [&](const ComponentInfo& c, void* data) {
            for (const FieldInfo& f : c.fields) {
                if (f.kind != FieldKind::EntityRef) continue;
                Guid& ref = f.ref<Guid>(data);
                if (!ref.isNil() && out.findById(ref).isNull()) {
                    ref = Guid{};
                    ++cleared;
                }
            }
        });
    }
    if (clearedReferences != nullptr) *clearedReferences = cleared;
    out.registry().remove<PrefabInstance>(copy);
    if (auto* t = out.tryGet<Transform>(copy)) t->position = {};
    return out;
}

Result<InstanceUpdate> updateInstance(Scene& scene, Entity root, const Scene& previous, const Scene& updated,
                                      bool keepChanges) {
    if (!scene.isAlive(root)) return makeError(ErrorCode::NotFound, "The instance does not exist");
    if (previous.entities().empty() || updated.entities().empty()) {
        return makeError(ErrorCode::InvalidData, "The prefab is empty");
    }
    const Entity previousRoot = previous.entities().front();
    const Entity updatedRoot = updated.entities().front();
    const Guid rootId = idOf(scene, root);
    const Guid oldKey = instanceKey(rootId, idOf(previous, previousRoot));
    const Guid newKey = instanceKey(rootId, idOf(updated, updatedRoot));
    const auto* link = scene.tryGet<PrefabInstance>(root);
    const assets::AssetId prefabId = link != nullptr ? link->prefab : assets::AssetId{};

    // 1. Which instance entity came from which prefab entity. An entity
    // counts as the prefab's only where it still sits under the same parent;
    // anything else is the instance's own (added, or moved elsewhere).
    std::unordered_map<std::uint32_t, Guid> prefabIdOf; // instance entity index -> previous prefab id
    std::vector<std::pair<Guid, Entity>> mapped;        // previous prefab id -> instance entity
    struct Extra {
        Entity entity;
        Guid attachTo; // prefab id of the parent it hangs under
    };
    std::vector<Extra> extras;
    for (const Entity e : scene.subtree(root)) {
        if (e == root) {
            prefabIdOf.emplace(e.index, idOf(previous, previousRoot));
            mapped.emplace_back(idOf(previous, previousRoot), e);
            continue;
        }
        const auto parentMapping = prefabIdOf.find(scene.parent(e).index);
        if (parentMapping == prefabIdOf.end()) continue; // inside an extra tree: moves with it
        const Guid candidate = instanceEntityId(oldKey, idOf(scene, e));
        const Entity original = previous.findById(candidate);
        if (!original.isNull() && idOf(previous, previous.parent(original)) == parentMapping->second) {
            prefabIdOf.emplace(e.index, candidate);
            mapped.emplace_back(candidate, e);
        } else {
            extras.push_back({e, parentMapping->second});
        }
    }
    if (!keepChanges) extras.clear();

    // 2. What this instance changed.
    std::vector<std::pair<Guid, EntityChanges>> changes;
    std::unordered_set<Guid> deleted; // prefab entities deleted in this instance (tree roots)
    if (keepChanges) {
        for (const auto& [prefabEntityId, e] : mapped) {
            EntityChanges ch = diffEntity(scene, e, previous, previous.findById(prefabEntityId), e == root);
            if (ch.count() > 0) changes.emplace_back(prefabEntityId, std::move(ch));
        }
        std::unordered_set<Guid> present;
        for (const auto& m : mapped) present.insert(m.first);
        for (const Entity p : previous.entities()) {
            if (p == previousRoot || present.contains(idOf(previous, p))) continue;
            if (present.contains(idOf(previous, previous.parent(p)))) deleted.insert(idOf(previous, p));
        }
    }

    // 3. The updated prefab without the entities this instance deleted.
    std::optional<Scene> filtered;
    if (!deleted.empty()) {
        filtered = updated;
        for (const Guid& p : deleted) {
            const Entity e = filtered->findById(p);
            if (!e.isNull() && e != filtered->entities().front()) filtered->destroyEntity(e);
        }
    }
    const Scene& source = filtered ? *filtered : updated;

    // Check the new ids are free before changing anything: only the
    // instance's own entities (not the trees added to it) give theirs up.
    std::unordered_set<std::uint32_t> replaced;
    for (const Entity e : scene.subtree(root)) replaced.insert(e.index);
    for (const Extra& x : extras) {
        for (const Entity e : scene.subtree(x.entity)) replaced.erase(e.index);
    }
    for (const Entity p : source.entities()) {
        const Entity holder = scene.findById(instanceEntityId(newKey, idOf(source, p)));
        if (!holder.isNull() && !replaced.contains(holder.index)) {
            return makeError(ErrorCode::InvalidData, "Cannot update '{}': another entity has one of its ids",
                             scene.get<EntityInfo>(root).name);
        }
    }

    // 4. Rebuild from the updated prefab.
    const Entity parent = scene.parent(root);
    Entity before = kNullEntity;
    {
        const std::vector<Entity> siblings = scene.children(parent);
        const auto it = std::find(siblings.begin(), siblings.end(), root);
        if (it != siblings.end() && it + 1 != siblings.end()) before = *(it + 1);
    }
    const std::string rootName = scene.get<EntityInfo>(root).name;
    const std::optional<Vec2> rootPosition =
        scene.tryGet<Transform>(root) != nullptr ? std::optional<Vec2>(scene.get<Transform>(root).position) : std::nullopt;
    for (const Extra& x : extras) (void)scene.setParent(x.entity, kNullEntity);
    std::unordered_set<Guid> oldIds;
    for (const Entity e : scene.subtree(root)) oldIds.insert(idOf(scene, e));
    scene.destroyEntity(root);

    auto created = instantiatePrefab(scene, source, prefabId, rootId, parent, before);
    if (!created) {
        // Cannot happen after the checks above; the instance is lost, so say so loudly.
        return makeError(created.error().code, "Updating '{}' failed: {}", rootName, created.error().message);
    }
    InstanceUpdate result;
    result.root = created.value();
    if (prefabId.isNil()) scene.registry().remove<PrefabInstance>(result.root);

    // 5. Carry the instance's changes over.
    for (const auto& [prefabEntityId, ch] : changes) {
        const Entity e = scene.findById(instanceEntityId(newKey, prefabEntityId));
        if (e.isNull()) continue; // removed from the prefab
        result.keptChanges += applyChanges(scene, e, ch, scene.get<EntityInfo>(e).name, result.warnings);
    }
    if (!keepChanges) {
        scene.get<EntityInfo>(result.root).name = rootName;
        if (rootPosition) {
            if (auto* t = scene.tryGet<Transform>(result.root)) t->position = *rootPosition;
        }
    }
    for (const Extra& x : extras) {
        Entity target = scene.findById(instanceEntityId(newKey, x.attachTo));
        if (target.isNull() || (target != result.root && !scene.isAncestor(result.root, target))) target = result.root;
        if (scene.setParent(x.entity, target)) {
            ++result.keptEntities;
        } else {
            result.warnings.push_back(std::format("'{}' could not be put back into the instance",
                                                  scene.get<EntityInfo>(x.entity).name));
        }
    }

    // 6. References: ids that changed are remapped, ids that are gone are cleared.
    if (oldKey != newKey) {
        std::vector<std::pair<Guid, Guid>> remap;
        for (const auto& m : mapped) remap.emplace_back(instanceEntityId(oldKey, m.first), instanceEntityId(newKey, m.first));
        remapEntityReferences(scene, scene.entities(), remap);
    }
    std::unordered_set<Guid> gone;
    for (const Guid& id : oldIds) {
        if (scene.findById(id).isNull()) gone.insert(id);
    }
    clearReferences(scene, gone);

    for (const Entity e : scene.subtree(result.root)) {
        const auto problems = validateEntity(scene, e, nullptr);
        if (!problems.empty()) {
            result.warnings.push_back(std::format("'{}': {} {}", scene.get<EntityInfo>(e).name, problems.front().component,
                                                  problems.front().message));
        }
    }
    return result;
}

std::vector<Entity> findInstances(const Scene& scene, assets::AssetId prefab) {
    std::vector<Entity> out;
    for (const Entity e : scene.entities()) {
        const auto* link = scene.tryGet<PrefabInstance>(e);
        if (link != nullptr && link->prefab == prefab) out.push_back(e);
    }
    return out;
}

} // namespace px::project
