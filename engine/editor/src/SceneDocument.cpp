#include "px/editor/SceneDocument.hpp"

#include "px/editor/EditorProject.hpp"
#include "px/project/SceneFormat.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <unordered_set>

namespace px::editor {

using namespace px::scene;

namespace {

constexpr std::size_t kMaxUndoSteps = 500;

std::string defaultName(EntityPreset preset) {
    switch (preset) {
        case EntityPreset::Empty: return "Entity";
        case EntityPreset::Group: return "Group";
        case EntityPreset::Sprite: return "Sprite";
        case EntityPreset::Tilemap: return "Tilemap";
        case EntityPreset::SolidBlock: return "Block";
        case EntityPreset::Trigger: return "Trigger";
        case EntityPreset::Player: return "Player";
        case EntityPreset::Camera: return "Camera";
    }
    return "Entity";
}

// Selected entities whose ancestors are not selected (moving or deleting
// a parent already covers its children).
std::vector<Entity> topLevel(const Scene& scene, const std::vector<Entity>& selection) {
    std::vector<Entity> out;
    for (const Entity e : selection) {
        const bool nested = std::any_of(selection.begin(), selection.end(),
                                        [&](Entity other) { return other != e && scene.isAncestor(other, e); });
        if (!nested) out.push_back(e);
    }
    return out;
}

} // namespace

std::string_view presetName(EntityPreset preset) {
    switch (preset) {
        case EntityPreset::Empty: return "Empty Entity";
        case EntityPreset::Group: return "Group";
        case EntityPreset::Sprite: return "Sprite";
        case EntityPreset::Tilemap: return "Tilemap";
        case EntityPreset::SolidBlock: return "Solid Block";
        case EntityPreset::Trigger: return "Trigger Area";
        case EntityPreset::Player: return "Player";
        case EntityPreset::Camera: return "Camera";
    }
    return "Entity";
}

std::size_t estimateSceneBytes(const Scene& scene) {
    std::size_t bytes = 1024 + scene.entityCount() * 384;
    for (const Entity e : scene.entities()) {
        if (const auto* map = scene.tryGet<Tilemap>(e)) bytes += map->tiles.size() * sizeof(int);
        if (const auto* anim = scene.tryGet<SpriteAnimation>(e)) bytes += anim->frames.size() * sizeof(Rect);
        if (const auto* scripts = scene.tryGet<Scripts>(e)) {
            for (const auto& s : scripts->items) bytes += 64 + s.properties.size() * 96;
        }
        if (const auto* tags = scene.tryGet<Tags>(e)) bytes += tags->values.size() * 48;
    }
    return bytes;
}

SceneDocument::SceneDocument(assets::AssetId sceneId, Scene scene) : m_sceneId(sceneId), m_scene(std::move(scene)) {}

// --- Transactions ------------------------------------------------------------------------------

SceneDocument::Snapshot SceneDocument::capture(std::string label) const {
    Snapshot s;
    s.scene = m_scene;
    s.selection = m_selection;
    s.primary = m_primary;
    s.version = m_version;
    s.label = std::move(label);
    s.bytes = estimateSceneBytes(m_scene);
    return s;
}

void SceneDocument::restore(Snapshot& snapshot) {
    m_scene = std::move(snapshot.scene);
    m_selection = std::move(snapshot.selection);
    m_primary = snapshot.primary;
    m_version = snapshot.version;
}

void SceneDocument::beginEdit(std::string_view label) {
    if (m_editDepth++ > 0) return;
    m_pending = std::make_unique<Snapshot>(capture(std::string(label)));
    m_changesAtBegin = m_changes;
}

void SceneDocument::endEdit() {
    if (m_editDepth == 0) return;
    if (--m_editDepth > 0) return;
    if (m_pending && m_changes != m_changesAtBegin) {
        for (const Snapshot& s : m_redo) m_undoBytes -= s.bytes;
        m_redo.clear();
        m_undoBytes += m_pending->bytes;
        m_undo.push_back(std::move(*m_pending));
        m_version = m_nextVersion++;
        trimUndo();
    }
    m_pending.reset();
}

void SceneDocument::cancelEdit() {
    if (m_editDepth == 0) return;
    m_editDepth = 0;
    if (m_pending) restore(*m_pending);
    m_pending.reset();
}

void SceneDocument::trimUndo() {
    while (!m_undo.empty() && (m_undoBytes > m_undoLimit || m_undo.size() > kMaxUndoSteps)) {
        m_undoBytes -= m_undo.front().bytes;
        m_undo.erase(m_undo.begin());
    }
}

std::string SceneDocument::undoLabel() const { return m_undo.empty() ? std::string() : m_undo.back().label; }
std::string SceneDocument::redoLabel() const { return m_redo.empty() ? std::string() : m_redo.back().label; }

void SceneDocument::undo() {
    if (m_undo.empty() || inEdit()) return;
    Snapshot previous = std::move(m_undo.back());
    m_undo.pop_back();
    Snapshot current = capture(previous.label);
    m_undoBytes -= previous.bytes;
    m_undoBytes += current.bytes;
    restore(previous);
    m_redo.push_back(std::move(current));
}

void SceneDocument::redo() {
    if (m_redo.empty() || inEdit()) return;
    Snapshot next = std::move(m_redo.back());
    m_redo.pop_back();
    Snapshot current = capture(next.label);
    m_undoBytes -= next.bytes;
    m_undoBytes += current.bytes;
    restore(next);
    m_undo.push_back(std::move(current));
}

// --- Saving ---------------------------------------------------------------------------------------

Status SceneDocument::save(EditorProject& project) {
    if (auto saved = project.saveScene(m_sceneId, m_scene); !saved) return saved;
    m_savedVersion = m_version;
    return {};
}

std::vector<std::pair<Entity, ComponentProblem>> SceneDocument::problems(const assets::AssetRegistry* assets) const {
    std::vector<std::pair<Entity, ComponentProblem>> out;
    for (const Entity e : m_scene.entities()) {
        for (auto& p : validateEntity(m_scene, e, assets)) out.emplace_back(e, std::move(p));
    }
    return out;
}

// --- Selection --------------------------------------------------------------------------------------

std::vector<Entity> SceneDocument::selection() const {
    std::vector<Entity> out;
    for (const Guid& id : m_selection) {
        const Entity e = m_scene.findById(id);
        if (!e.isNull()) out.push_back(e);
    }
    std::sort(out.begin(), out.end(),
              [&](Entity a, Entity b) { return m_scene.orderIndex(a) < m_scene.orderIndex(b); });
    return out;
}

Entity SceneDocument::primarySelection() const {
    const Entity e = m_scene.findById(m_primary);
    if (!e.isNull() && isSelected(e)) return e;
    const auto all = selection();
    return all.empty() ? kNullEntity : all.back();
}

bool SceneDocument::isSelected(Entity entity) const {
    const auto* info = m_scene.tryGet<EntityInfo>(entity);
    return info != nullptr && std::find(m_selection.begin(), m_selection.end(), info->id) != m_selection.end();
}

void SceneDocument::select(Entity entity, SelectMode mode) {
    const auto* info = m_scene.tryGet<EntityInfo>(entity);
    if (info == nullptr) {
        if (mode == SelectMode::Replace) clearSelection();
        return;
    }
    const Guid id = info->id;
    const auto it = std::find(m_selection.begin(), m_selection.end(), id);
    switch (mode) {
        case SelectMode::Replace:
            m_selection = {id};
            m_primary = id;
            break;
        case SelectMode::Add:
            if (it == m_selection.end()) m_selection.push_back(id);
            m_primary = id;
            break;
        case SelectMode::Toggle:
            if (it == m_selection.end()) {
                m_selection.push_back(id);
                m_primary = id;
            } else {
                m_selection.erase(it);
                m_primary = m_selection.empty() ? Guid{} : m_selection.back();
            }
            break;
    }
}

void SceneDocument::setSelection(const std::vector<Entity>& entities) {
    m_selection.clear();
    for (const Entity e : entities) {
        if (const auto* info = m_scene.tryGet<EntityInfo>(e)) m_selection.push_back(info->id);
    }
    m_primary = m_selection.empty() ? Guid{} : m_selection.back();
}

void SceneDocument::clearSelection() {
    m_selection.clear();
    m_primary = Guid{};
}

// --- Operations -----------------------------------------------------------------------------------------

std::string SceneDocument::uniqueName(std::string_view wanted) const {
    const auto used = [&](const std::string& name) { return !m_scene.findByName(name).isNull(); };
    std::string base(wanted.empty() ? std::string_view("Entity") : wanted);
    if (!used(base)) return base;
    long long number = 2;
    const std::size_t space = base.find_last_of(' ');
    if (space != std::string::npos && space + 1 < base.size() &&
        std::all_of(base.begin() + static_cast<std::ptrdiff_t>(space) + 1, base.end(),
                    [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }) &&
        base.size() - space - 1 < 10) {
        number = std::stoll(base.substr(space + 1)) + 1;
        base = base.substr(0, space);
    }
    for (;; ++number) {
        std::string candidate = std::format("{} {}", base, number);
        if (!used(candidate)) return candidate;
    }
}

Entity SceneDocument::createEntity(EntityPreset preset, std::string name, Entity parent, Vec2 position,
                                   assets::AssetId texture) {
    AutoEdit edit(*this, std::format("Create {}", presetName(preset)));
    const Entity e = m_scene.createEntity(uniqueName(name.empty() ? defaultName(preset) : name), parent);
    if (e.isNull()) return e;
    if (preset != EntityPreset::Group) {
        Transform t;
        t.position = position;
        m_scene.set(e, t);
        // `position` is in world space.
        if (!parent.isNull()) m_scene.setWorldPosition(e, position);
    }
    switch (preset) {
        case EntityPreset::Empty:
        case EntityPreset::Group: break;
        case EntityPreset::Sprite: {
            Sprite sprite;
            sprite.texture = texture;
            m_scene.set(e, sprite);
            break;
        }
        case EntityPreset::Tilemap: {
            Tilemap map;
            map.tileset = texture;
            map.width = 20;
            map.height = 10;
            map.tiles.assign(static_cast<std::size_t>(map.width * map.height), -1);
            m_scene.set(e, std::move(map));
            break;
        }
        case EntityPreset::SolidBlock: {
            if (!texture.isNil()) {
                Sprite sprite;
                sprite.texture = texture;
                m_scene.set(e, sprite);
            }
            m_scene.set(e, BoxCollider{});
            break;
        }
        case EntityPreset::Trigger: {
            BoxCollider c;
            c.size = {32, 32};
            c.isTrigger = true;
            m_scene.set(e, c);
            break;
        }
        case EntityPreset::Player: {
            if (!texture.isNil()) {
                Sprite sprite;
                sprite.texture = texture;
                sprite.layer = 10;
                m_scene.set(e, sprite);
            }
            BoxCollider c;
            c.size = {10, 14};
            c.offset = {0, 1};
            m_scene.set(e, c);
            m_scene.set(e, KinematicBody{});
            m_scene.set(e, PlatformerController{});
            m_scene.set(e, Player{});
            // Cameras that follow nothing yet follow the new player.
            const Guid id = m_scene.tryGet<EntityInfo>(e)->id;
            for (const Entity other : m_scene.entities()) {
                if (auto* cam = m_scene.tryGet<Camera>(other); cam != nullptr && cam->follow.isNil()) cam->follow = id;
            }
            break;
        }
        case EntityPreset::Camera: {
            Camera cam;
            for (const Entity other : m_scene.entities()) {
                if (m_scene.tryGet<Player>(other) != nullptr) {
                    cam.follow = m_scene.tryGet<EntityInfo>(other)->id;
                    break;
                }
            }
            m_scene.set(e, cam);
            break;
        }
    }
    markChanged();
    select(e);
    return e;
}

void SceneDocument::rename(Entity entity, std::string name) {
    auto* info = m_scene.tryGet<EntityInfo>(entity);
    if (info == nullptr || info->name == name) return;
    AutoEdit edit(*this, "Rename");
    info->name = std::move(name);
    markChanged();
}

void SceneDocument::deleteSelection() {
    const auto roots = topLevel(m_scene, selection());
    if (roots.empty()) return;
    AutoEdit edit(*this, roots.size() == 1 ? "Delete Entity" : "Delete Entities");
    std::vector<Guid> removed;
    for (const Entity root : roots) {
        for (const Entity e : m_scene.subtree(root)) removed.push_back(m_scene.tryGet<EntityInfo>(e)->id);
        m_scene.destroyEntity(root);
    }
    for (const Guid& id : removed) (void)clearEntityReferences(m_scene, id);
    clearSelection();
    markChanged();
}

std::vector<Entity> SceneDocument::duplicateSelection() {
    const auto roots = topLevel(m_scene, selection());
    std::vector<Entity> copies;
    if (roots.empty()) return copies;
    AutoEdit edit(*this, roots.size() == 1 ? "Duplicate Entity" : "Duplicate Entities");
    std::vector<std::pair<Guid, Guid>> remap;
    std::vector<Entity> copiedEntities;
    for (const Entity root : roots) {
        const Entity parent = m_scene.parent(root);
        // Insert right after the original's subtree.
        const std::size_t after = m_scene.orderIndex(root) + m_scene.subtree(root).size();
        Entity before = kNullEntity;
        if (after < m_scene.entityCount() && m_scene.parent(m_scene.entities()[after]) == parent) {
            before = m_scene.entities()[after];
        }
        const Entity copy = m_scene.copySubtree(m_scene, root, parent, before);
        if (copy.isNull()) continue;
        m_scene.tryGet<EntityInfo>(copy)->name = uniqueName(m_scene.tryGet<EntityInfo>(root)->name);
        const auto originals = m_scene.subtree(root);
        const auto copied = m_scene.subtree(copy);
        for (std::size_t i = 0; i < originals.size() && i < copied.size(); ++i) {
            remap.emplace_back(m_scene.tryGet<EntityInfo>(originals[i])->id, m_scene.tryGet<EntityInfo>(copied[i])->id);
        }
        copiedEntities.insert(copiedEntities.end(), copied.begin(), copied.end());
        copies.push_back(copy);
    }
    // References between duplicated entities point at the copies (a copied
    // camera follows the copied player), like copy and paste.
    remapEntityReferences(m_scene, copiedEntities, remap);
    setSelection(copies);
    markChanged();
    return copies;
}

std::string SceneDocument::copySelection() const {
    const auto roots = topLevel(m_scene, selection());
    return roots.empty() ? std::string() : project::serializeEntities(m_scene, roots);
}

Result<std::vector<Entity>> SceneDocument::paste(std::string_view text, const assets::AssetRegistry* assets,
                                                 Entity parent) {
    AutoEdit edit(*this, "Paste");
    auto pasted = project::pasteEntities(m_scene, text, assets, parent);
    if (!pasted) return pasted.error();
    for (const Entity e : pasted.value()) {
        auto* info = m_scene.tryGet<EntityInfo>(e);
        // Only rename when the name is taken by another entity.
        std::string name = info->name;
        info->name.clear();
        info->name = m_scene.findByName(name).isNull() ? name : uniqueName(name);
    }
    setSelection(pasted.value());
    if (!pasted->empty()) markChanged();
    return pasted;
}

Status SceneDocument::setParent(Entity entity, Entity parent, Entity before) {
    AutoEdit edit(*this, "Change Parent");
    auto status = m_scene.setParent(entity, parent, before, true);
    if (status) markChanged();
    return status;
}

Status SceneDocument::addComponent(Entity entity, const ComponentInfo& component) {
    if (!m_scene.isAlive(entity)) return makeError(ErrorCode::NotFound, "The entity does not exist");
    if (component.has(m_scene, entity)) {
        return makeError(ErrorCode::InvalidArgument, "The entity already has a {}", component.name);
    }
    AutoEdit edit(*this, std::format("Add {}", component.name));
    for (const ComponentInfo* required : missingRequirements(m_scene, entity, component)) required->add(m_scene, entity);
    component.add(m_scene, entity);
    markChanged();
    return {};
}

Status SceneDocument::removeComponent(Entity entity, const ComponentInfo& component) {
    if (!component.has(m_scene, entity)) return {};
    const auto users = dependents(m_scene, entity, component);
    if (!users.empty()) {
        return makeError(ErrorCode::InvalidArgument, "{} is needed by {}; remove that first", component.name,
                         users.front()->name);
    }
    AutoEdit edit(*this, std::format("Remove {}", component.name));
    component.remove(m_scene, entity);
    markChanged();
    return {};
}

Status SceneDocument::attachScript(Entity entity, assets::AssetId script) {
    if (!m_scene.isAlive(entity)) return makeError(ErrorCode::NotFound, "The entity does not exist");
    auto* existing = m_scene.tryGet<Scripts>(entity);
    if (existing != nullptr && existing->items.size() >= kMaxScriptsPerEntity) {
        return makeError(ErrorCode::LimitExceeded, "An entity can have at most {} scripts", kMaxScriptsPerEntity);
    }
    AutoEdit edit(*this, "Add Script");
    if (existing == nullptr) existing = &m_scene.set(entity, Scripts{});
    existing->items.push_back(AttachedScript{script, {}});
    markChanged();
    return {};
}

void SceneDocument::detachScript(Entity entity, std::size_t index) {
    auto* scripts = m_scene.tryGet<Scripts>(entity);
    if (scripts == nullptr || index >= scripts->items.size()) return;
    AutoEdit edit(*this, "Remove Script");
    scripts->items.erase(scripts->items.begin() + static_cast<std::ptrdiff_t>(index));
    markChanged();
}

void SceneDocument::setScriptProperty(Entity entity, std::size_t index, std::string name, ScriptValue value) {
    auto* scripts = m_scene.tryGet<Scripts>(entity);
    if (scripts == nullptr || index >= scripts->items.size()) return;
    auto& props = scripts->items[index].properties;
    const auto it = std::find_if(props.begin(), props.end(), [&](const ScriptProperty& p) { return p.name == name; });
    if (it != props.end() && it->value == value) return;
    AutoEdit edit(*this, "Set Script Property");
    if (it != props.end()) {
        it->value = std::move(value);
    } else {
        props.push_back({std::move(name), std::move(value)});
    }
    markChanged();
}

void SceneDocument::resetScriptProperty(Entity entity, std::size_t index, std::string_view name) {
    auto* scripts = m_scene.tryGet<Scripts>(entity);
    if (scripts == nullptr || index >= scripts->items.size()) return;
    auto& props = scripts->items[index].properties;
    const auto it = std::find_if(props.begin(), props.end(), [&](const ScriptProperty& p) { return p.name == name; });
    if (it == props.end()) return;
    AutoEdit edit(*this, "Reset Script Property");
    props.erase(it);
    markChanged();
}

void SceneDocument::translateSelection(Vec2 worldDelta) {
    if (worldDelta.x == 0.0f && worldDelta.y == 0.0f) return;
    const auto roots = topLevel(m_scene, selection());
    AutoEdit edit(*this, "Move");
    for (const Entity e : roots) {
        if (m_scene.tryGet<Transform>(e) == nullptr) continue;
        m_scene.setWorldPosition(e, m_scene.worldPosition(e) + worldDelta);
        markChanged();
    }
}

void SceneDocument::resizeTilemap(Entity entity, int width, int height) {
    auto* map = m_scene.tryGet<Tilemap>(entity);
    if (map == nullptr) return;
    width = std::clamp(width, 1, kMaxTilemapDimension);
    height = std::clamp(height, 1, kMaxTilemapDimension);
    if (width == map->width && height == map->height) return;
    AutoEdit edit(*this, "Resize Tilemap");
    std::vector<int> tiles(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), -1);
    for (int y = 0; y < std::min(height, map->height); ++y) {
        for (int x = 0; x < std::min(width, map->width); ++x) {
            tiles[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] =
                map->tileAt(x, y);
        }
    }
    map->width = width;
    map->height = height;
    map->tiles = std::move(tiles);
    markChanged();
}

bool SceneDocument::setTile(Entity entity, int x, int y, int tile) {
    auto* map = m_scene.tryGet<Tilemap>(entity);
    if (map == nullptr || x < 0 || y < 0 || x >= map->width || y >= map->height) return false;
    int& slot = map->tiles[static_cast<std::size_t>(y) * static_cast<std::size_t>(map->width) + static_cast<std::size_t>(x)];
    if (slot == tile) return false;
    AutoEdit edit(*this, tile < 0 ? "Erase Tiles" : "Paint Tiles");
    slot = tile;
    markChanged();
    return true;
}

} // namespace px::editor
