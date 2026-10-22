#include "px/scene/Scene.hpp"

#include <algorithm>

namespace px::scene {

Entity Scene::createEntity(std::string name) {
    Guid id = Guid::generate();
    while (m_byId.contains(id)) id = Guid::generate();
    return createEntityWithId(id, std::move(name)).value();
}

Result<Entity> Scene::createEntityWithId(Guid id, std::string name) {
    if (id.isNil()) {
        return makeError(ErrorCode::InvalidArgument, "Entity '{}' has a nil id", name);
    }
    if (const auto it = m_byId.find(id); it != m_byId.end()) {
        const auto* existing = m_registry.tryGet<EntityInfo>(it->second);
        return makeError(ErrorCode::InvalidData, "Entity id {} is used by both '{}' and '{}'", id.toString(),
                         existing ? existing->name : std::string("?"), name);
    }
    const Entity e = m_registry.create();
    m_registry.set<EntityInfo>(e, EntityInfo{id, std::move(name)});
    m_byId.emplace(id, e);
    m_order.push_back(e);
    return e;
}

void Scene::destroyEntity(Entity entity) {
    if (!m_registry.isAlive(entity)) return;
    if (const auto* info = m_registry.tryGet<EntityInfo>(entity)) {
        m_byId.erase(info->id);
    }
    std::erase(m_order, entity);
    m_registry.destroy(entity);
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

} // namespace px::scene
