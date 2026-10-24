#include "px/scene/Scene.hpp"

#include <algorithm>
#include <array>
#include <unordered_map>

namespace px::scene {

namespace {

float safeDivide(float value, float by) { return by == 0.0f ? 0.0f : value / by; }

std::string displayName(const Scene& scene, Entity e) {
    const auto* info = scene.tryGet<EntityInfo>(e);
    return info != nullptr ? info->name : std::string("?");
}

} // namespace

Entity Scene::createEntity(std::string name, Entity parent) {
    Guid id = Guid::generate();
    while (m_byId.contains(id)) id = Guid::generate();
    auto created = createEntityWithId(id, std::move(name), parent);
    return created ? created.value() : kNullEntity;
}

Result<Entity> Scene::createEntityWithId(Guid id, std::string name, Entity parent) {
    if (id.isNil()) {
        return makeError(ErrorCode::InvalidArgument, "Entity '{}' has a nil id", name);
    }
    if (const auto it = m_byId.find(id); it != m_byId.end()) {
        const auto* existing = m_registry.tryGet<EntityInfo>(it->second);
        return makeError(ErrorCode::InvalidData, "Entity id {} is used by both '{}' and '{}'", id.toString(),
                         existing ? existing->name : std::string("?"), name);
    }
    if (!parent.isNull()) {
        if (!isAlive(parent)) {
            return makeError(ErrorCode::InvalidArgument, "Entity '{}': the parent entity does not exist", name);
        }
        if (depth(parent) + 1 > kMaxHierarchyDepth) {
            return makeError(ErrorCode::LimitExceeded, "Entity '{}' would be nested deeper than {} levels", name,
                             kMaxHierarchyDepth);
        }
    }
    const Entity e = m_registry.create();
    m_registry.set<EntityInfo>(e, EntityInfo{id, std::move(name)});
    m_byId.emplace(id, e);
    setParentLink(e, parent);
    if (parent.isNull()) {
        m_order.push_back(e);
    } else {
        const auto at = static_cast<std::ptrdiff_t>(subtreeEnd(orderIndex(parent)));
        m_order.insert(m_order.begin() + at, e);
    }
    return e;
}

void Scene::destroyEntity(Entity entity) {
    if (!m_registry.isAlive(entity)) return;
    const std::size_t begin = orderIndex(entity);
    const std::size_t end = subtreeEnd(begin);
    const std::vector<Entity> doomed(m_order.begin() + static_cast<std::ptrdiff_t>(begin),
                                     m_order.begin() + static_cast<std::ptrdiff_t>(end));
    m_order.erase(m_order.begin() + static_cast<std::ptrdiff_t>(begin),
                  m_order.begin() + static_cast<std::ptrdiff_t>(end));
    for (const Entity e : doomed) {
        if (const auto* info = m_registry.tryGet<EntityInfo>(e)) m_byId.erase(info->id);
        m_parents[e.index] = kNullEntity;
        m_registry.destroy(e);
    }
}

Entity Scene::findById(Guid id) const {
    const auto it = m_byId.find(id);
    return it == m_byId.end() ? kNullEntity : it->second;
}

Entity Scene::findByName(std::string_view name) const {
    for (const Entity e : m_order) {
        const auto* info = m_registry.tryGet<EntityInfo>(e);
        if (info && info->name == name) return e;
    }
    return kNullEntity;
}

// --- Hierarchy -----------------------------------------------------------------------------

void Scene::setParentLink(Entity child, Entity parent) {
    if (child.index >= m_parents.size()) m_parents.resize(child.index + 1, kNullEntity);
    m_parents[child.index] = parent;
}

Entity Scene::parent(Entity entity) const {
    if (!isAlive(entity) || entity.index >= m_parents.size()) return kNullEntity;
    return m_parents[entity.index];
}

std::vector<Entity> Scene::children(Entity entity) const {
    std::vector<Entity> out;
    if (entity.isNull()) {
        for (const Entity e : m_order) {
            if (parent(e).isNull()) out.push_back(e);
        }
        return out;
    }
    const std::size_t begin = orderIndex(entity);
    if (begin == npos) return out;
    const std::size_t end = subtreeEnd(begin);
    for (std::size_t i = begin + 1; i < end; ++i) {
        if (parent(m_order[i]) == entity) out.push_back(m_order[i]);
    }
    return out;
}

bool Scene::hasChildren(Entity entity) const {
    const std::size_t index = orderIndex(entity);
    return index != npos && index + 1 < m_order.size() && parent(m_order[index + 1]) == entity;
}

bool Scene::isAncestor(Entity ancestor, Entity entity) const {
    if (ancestor.isNull()) return false;
    int guard = 0;
    for (Entity p = parent(entity); !p.isNull() && guard <= kMaxHierarchyDepth; p = parent(p), ++guard) {
        if (p == ancestor) return true;
    }
    return false;
}

int Scene::depth(Entity entity) const {
    int d = 0;
    for (Entity p = parent(entity); !p.isNull() && d <= kMaxHierarchyDepth; p = parent(p)) ++d;
    return d;
}

std::vector<Entity> Scene::subtree(Entity entity) const {
    const std::size_t begin = orderIndex(entity);
    if (begin == npos) return {};
    const std::size_t end = subtreeEnd(begin);
    return {m_order.begin() + static_cast<std::ptrdiff_t>(begin), m_order.begin() + static_cast<std::ptrdiff_t>(end)};
}

std::size_t Scene::orderIndex(Entity entity) const {
    if (!isAlive(entity)) return npos;
    const auto it = std::find(m_order.begin(), m_order.end(), entity);
    return it == m_order.end() ? npos : static_cast<std::size_t>(it - m_order.begin());
}

std::size_t Scene::subtreeEnd(std::size_t index) const {
    // Subtrees are contiguous: the subtree ends at the first following
    // entity that is not deeper than its root.
    const int rootDepth = depth(m_order[index]);
    std::size_t end = index + 1;
    while (end < m_order.size() && depth(m_order[end]) > rootDepth) ++end;
    return end;
}

int Scene::subtreeHeight(Entity entity) const {
    const std::size_t begin = orderIndex(entity);
    if (begin == npos) return 0;
    const int rootDepth = depth(entity);
    int height = 0;
    const std::size_t end = subtreeEnd(begin);
    for (std::size_t i = begin + 1; i < end; ++i) height = std::max(height, depth(m_order[i]) - rootDepth);
    return height;
}

Status Scene::setParent(Entity entity, Entity newParent, Entity before, bool keepWorldTransform) {
    if (!isAlive(entity)) return makeError(ErrorCode::NotFound, "The entity does not exist");
    if (!newParent.isNull()) {
        if (!isAlive(newParent)) return makeError(ErrorCode::NotFound, "The new parent entity does not exist");
        if (newParent == entity || isAncestor(entity, newParent)) {
            return makeError(ErrorCode::InvalidArgument, "'{}' cannot become a child of itself or of its own child '{}'",
                             displayName(*this, entity), displayName(*this, newParent));
        }
        if (depth(newParent) + 1 + subtreeHeight(entity) > kMaxHierarchyDepth) {
            return makeError(ErrorCode::LimitExceeded, "Entities cannot be nested deeper than {} levels",
                             kMaxHierarchyDepth);
        }
    }
    if (!before.isNull() && (!isAlive(before) || before == entity || parent(before) != newParent)) {
        return makeError(ErrorCode::InvalidArgument, "The insertion point is not a child of the new parent");
    }

    Transform world;
    const bool keep = keepWorldTransform && tryGet<Transform>(entity) != nullptr;
    if (keep) world = worldTransform(entity);

    const std::size_t begin = orderIndex(entity);
    const std::size_t end = subtreeEnd(begin);
    const std::vector<Entity> block(m_order.begin() + static_cast<std::ptrdiff_t>(begin),
                                    m_order.begin() + static_cast<std::ptrdiff_t>(end));
    m_order.erase(m_order.begin() + static_cast<std::ptrdiff_t>(begin),
                  m_order.begin() + static_cast<std::ptrdiff_t>(end));
    setParentLink(entity, newParent);
    std::size_t insertAt = m_order.size();
    if (!before.isNull()) {
        insertAt = orderIndex(before);
    } else if (!newParent.isNull()) {
        insertAt = subtreeEnd(orderIndex(newParent));
    }
    m_order.insert(m_order.begin() + static_cast<std::ptrdiff_t>(insertAt), block.begin(), block.end());

    if (keep) {
        const Transform parentWorld = parentWorldTransform(entity);
        Transform& local = get<Transform>(entity);
        local.position = inverseTransformPoint(parentWorld, world.position);
        local.rotation = world.rotation - parentWorld.rotation;
        local.scale = {safeDivide(world.scale.x, parentWorld.scale.x), safeDivide(world.scale.y, parentWorld.scale.y)};
    }
    return {};
}

Status Scene::setParents(std::span<const std::pair<Entity, Entity>> links) {
    for (const auto& [child, newParent] : links) {
        if (!isAlive(child)) return makeError(ErrorCode::NotFound, "An entity in the hierarchy does not exist");
        if (!newParent.isNull() && !isAlive(newParent)) {
            return makeError(ErrorCode::NotFound, "The parent of '{}' does not exist", displayName(*this, child));
        }
        if (child == newParent) {
            return makeError(ErrorCode::InvalidData, "'{}' is its own parent", displayName(*this, child));
        }
    }
    const std::vector<Entity> previous = m_parents;
    for (const auto& [child, newParent] : links) setParentLink(child, newParent);

    // Depth-first walk from the roots, keeping the current sibling order.
    std::unordered_map<std::uint32_t, std::vector<Entity>> kids;
    std::vector<Entity> roots;
    for (const Entity e : m_order) {
        const Entity p = m_parents[e.index];
        if (p.isNull()) {
            roots.push_back(e);
        } else {
            kids[p.index].push_back(e);
        }
    }
    std::vector<Entity> order;
    order.reserve(m_order.size());
    std::vector<std::pair<Entity, int>> stack;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) stack.emplace_back(*it, 0);
    while (!stack.empty()) {
        const auto [e, d] = stack.back();
        stack.pop_back();
        if (d > kMaxHierarchyDepth) {
            m_parents = previous;
            return makeError(ErrorCode::LimitExceeded, "'{}' is nested deeper than {} levels", displayName(*this, e),
                             kMaxHierarchyDepth);
        }
        order.push_back(e);
        if (const auto k = kids.find(e.index); k != kids.end()) {
            for (auto it = k->second.rbegin(); it != k->second.rend(); ++it) stack.emplace_back(*it, d + 1);
        }
    }
    if (order.size() != m_order.size()) {
        // Entities not reachable from a root are part of a parent loop.
        std::vector<Entity> sorted = order;
        const auto less = [](Entity a, Entity b) { return a.index < b.index; };
        std::sort(sorted.begin(), sorted.end(), less);
        Entity inLoop = kNullEntity;
        for (const Entity e : m_order) {
            if (!std::binary_search(sorted.begin(), sorted.end(), e, less)) {
                inLoop = e;
                break;
            }
        }
        m_parents = previous;
        return makeError(ErrorCode::InvalidData, "the parents of '{}' form a loop (an entity cannot be its own ancestor)",
                         displayName(*this, inLoop));
    }
    m_order = std::move(order);
    return {};
}

// --- Transforms ----------------------------------------------------------------------------

Transform Scene::worldTransform(Entity entity) const {
    std::array<Entity, kMaxHierarchyDepth + 1> chain{};
    std::size_t count = 0;
    for (Entity e = entity; !e.isNull() && count < chain.size(); e = parent(e)) chain[count++] = e;
    Transform world;
    bool first = true;
    for (std::size_t i = count; i-- > 0;) {
        const auto* t = tryGet<Transform>(chain[i]);
        if (t == nullptr) continue;
        world = first ? *t : compose(world, *t);
        first = false;
    }
    return world;
}

Transform Scene::parentWorldTransform(Entity entity) const {
    const Entity p = parent(entity);
    return p.isNull() ? Transform{} : worldTransform(p);
}

void Scene::setWorldPosition(Entity entity, Vec2 position) {
    auto* t = tryGet<Transform>(entity);
    if (t == nullptr) return;
    t->position = parent(entity).isNull() ? position : inverseTransformPoint(parentWorldTransform(entity), position);
}

void Scene::setWorldRotation(Entity entity, float degrees) {
    auto* t = tryGet<Transform>(entity);
    if (t == nullptr) return;
    t->rotation = parent(entity).isNull() ? degrees : degrees - parentWorldTransform(entity).rotation;
}

// --- Copying -------------------------------------------------------------------------------

Entity Scene::copySubtree(const Scene& source, Entity root, Entity parentEntity, Entity before) {
    if (!source.isAlive(root)) return kNullEntity;
    if (!parentEntity.isNull() &&
        (!isAlive(parentEntity) || depth(parentEntity) + 1 + source.subtreeHeight(root) > kMaxHierarchyDepth)) {
        return kNullEntity;
    }
    const std::vector<Entity> nodes = source.subtree(root); // copied first: source may be this scene
    std::unordered_map<std::uint32_t, Entity> copies;
    Entity newRoot = kNullEntity;
    for (const Entity original : nodes) {
        const Entity copyParent = original == root ? parentEntity : copies.at(source.parent(original).index);
        const auto* info = source.tryGet<EntityInfo>(original);
        const Entity copy = createEntity(info != nullptr ? info->name : std::string(), copyParent);
        const auto* created = tryGet<EntityInfo>(copy);
        if (created == nullptr) return kNullEntity; // cannot happen: every entity has an EntityInfo
        const EntityInfo identity = *created;
        m_registry.copyComponents(source.m_registry, original, copy);
        set<EntityInfo>(copy, identity); // keep the new id
        copies.emplace(original.index, copy);
        if (original == root) newRoot = copy;
    }
    if (!before.isNull()) (void)setParent(newRoot, parentEntity, before);
    return newRoot;
}

Entity Scene::copySubtree(const Scene& source, Entity root, const std::function<Guid(const Guid&)>& makeId,
                          Entity parentEntity) {
    if (!source.isAlive(root)) return kNullEntity;
    if (!parentEntity.isNull() &&
        (!isAlive(parentEntity) || depth(parentEntity) + 1 + source.subtreeHeight(root) > kMaxHierarchyDepth)) {
        return kNullEntity;
    }
    const std::vector<Entity> nodes = source.subtree(root); // copied first: source may be this scene
    // Choose every id first so a clash leaves the scene unchanged.
    std::vector<Guid> ids;
    ids.reserve(nodes.size());
    for (const Entity original : nodes) {
        const auto* info = source.tryGet<EntityInfo>(original);
        const Guid id = makeId(info != nullptr ? info->id : Guid{});
        if (id.isNil() || m_byId.contains(id) || std::find(ids.begin(), ids.end(), id) != ids.end()) return kNullEntity;
        ids.push_back(id);
    }
    std::unordered_map<std::uint32_t, Entity> copies;
    Entity newRoot = kNullEntity;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const Entity original = nodes[i];
        const Entity copyParent = original == root ? parentEntity : copies.at(source.parent(original).index);
        const auto* info = source.tryGet<EntityInfo>(original);
        auto created = createEntityWithId(ids[i], info != nullptr ? info->name : std::string(), copyParent);
        if (!created) return kNullEntity; // cannot happen: ids were checked above
        const Entity copy = created.value();
        const EntityInfo identity = get<EntityInfo>(copy);
        m_registry.copyComponents(source.m_registry, original, copy);
        set<EntityInfo>(copy, identity); // keep the chosen id
        copies.emplace(original.index, copy);
        if (original == root) newRoot = copy;
    }
    return newRoot;
}

} // namespace px::scene
