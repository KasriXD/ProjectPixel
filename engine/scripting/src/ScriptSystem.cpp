#include "px/scripting/ScriptSystem.hpp"

#include "Internal.hpp"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <format>

namespace px::scripting {

using scene::Entity;

namespace {

const char* valueTypeName(const scene::ScriptValue& v) {
    switch (v.index()) {
        case 0: return "number";
        case 1: return "boolean";
        case 2: return "string";
        case 3: return "vector";
    }
    return "?";
}

void pushScriptValue(lua_State* L, const scene::ScriptValue& value) {
    std::visit(
        [L](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, double>) {
                lua_pushnumber(L, v);
            } else if constexpr (std::is_same_v<T, bool>) {
                lua_pushboolean(L, v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                lua_pushlstring(L, v.data(), v.size());
            } else {
                lua_pushvector(L, v.x, v.y, 0.0f);
            }
        },
        value);
}

} // namespace

ScriptSystem::ScriptSystem(scene::Scene& scene, std::shared_ptr<const ScriptLibrary> library, IScriptHost& host,
                           const ScriptSystemOptions& options)
    : m_scene(scene), m_library(std::move(library)), m_host(host), m_options(options) {}

ScriptSystem::~ScriptSystem() = default; // closing the VM releases every reference

Result<std::unique_ptr<ScriptSystem>> ScriptSystem::create(scene::Scene& scene,
                                                           std::shared_ptr<const ScriptLibrary> library,
                                                           IScriptHost& host, const ScriptSystemOptions& options) {
    if (!library) library = std::make_shared<ScriptLibrary>();
    std::unique_ptr<ScriptSystem> system(new ScriptSystem(scene, std::move(library), host, options));
    ScriptSystem* raw = system.get();
    auto vm = ScriptVm::create(options.realm, options.limits, options.seed,
                               [raw](lua_State* L) { registerScriptApi(L, *raw); }, raw);
    if (!vm) return vm.error();
    system->m_vm = std::move(vm.value());

    // Instances are created in hierarchy order, then attachment order; this
    // is also the order in which their handlers run. (The list is copied:
    // module code runs while instances are created.)
    const std::vector<Entity> entities = scene.entities();
    for (const Entity e : entities) {
        const auto* scripts = scene.tryGet<scene::Scripts>(e);
        if (scripts == nullptr) continue;
        const std::vector<scene::AttachedScript> items = scripts->items;
        for (const auto& attached : items) {
            if (auto status = system->createInstance(e, attached); !status) return status.error();
        }
    }
    return system;
}

Result<std::size_t> ScriptSystem::loadBehaviour(const CompiledScript& script) {
    if (const auto it = m_behaviourIndex.find(script.id); it != m_behaviourIndex.end()) return it->second;

    m_loadingScript = &script;
    auto moduleRef = m_vm->loadModule(script);
    m_loadingScript = nullptr;
    if (!moduleRef) {
        // Runtime errors already carry "file:line:"; prefix only bare messages.
        const std::string& message = moduleRef.error().message;
        if (message.starts_with(script.name)) return moduleRef.error();
        return makeError(moduleRef.error().code, "{}: {}", script.name, message);
    }

    Behaviour behaviour;
    behaviour.script = &script;
    behaviour.moduleRef = moduleRef.value();
    std::string problem;
    std::vector<std::string> typoWarnings;

    const ScriptVm::CallResult result = m_vm->runProtected([&](lua_State* L) {
        lua_getref(L, behaviour.moduleRef);
        if (!lua_istable(L, -1)) {
            problem = std::format("the script must end with `return <behaviour table>`, but it returned {}",
                                  luaL_typename(L, -1));
            return 0;
        }
        const int module = lua_gettop(L);

        std::vector<std::string> eventNames(kLifecycleEventNames.begin(), kLifecycleEventNames.end());
        for (std::size_t i = 0; i < kLifecycleEventCount; ++i) {
            lua_rawgetfield(L, module, kLifecycleEventNames[i]);
            if (lua_isfunction(L, -1)) {
                behaviour.handlers[i] = lua_ref(L, -1);
            } else if (!lua_isnil(L, -1)) {
                problem = std::format("'{}' must be a function (it is a {})", kLifecycleEventNames[i],
                                      luaL_typename(L, -1));
                return 0;
            }
            lua_pop(L, 1);
        }

        // Catch misspelled handlers ("Tik"), which would otherwise silently never run.
        lua_pushnil(L);
        while (lua_next(L, module) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING && lua_isfunction(L, -1)) {
                const std::string key = lua_tostring(L, -2);
                if (std::find(eventNames.begin(), eventNames.end(), key) == eventNames.end()) {
                    const std::string hint = suggestName(key, eventNames);
                    if (!hint.empty()) {
                        typoWarnings.push_back(std::format("function '{}' is not a lifecycle event{}", key, hint));
                    }
                }
            }
            lua_pop(L, 1);
        }

        lua_rawgetfield(L, module, "properties");
        if (lua_istable(L, -1)) {
            const int props = lua_gettop(L);
            lua_pushnil(L);
            while (lua_next(L, props) != 0) {
                if (lua_type(L, -2) != LUA_TSTRING) {
                    problem = "properties keys must be names";
                    return 0;
                }
                const std::string name = lua_tostring(L, -2);
                if (name == "entity") {
                    problem = "'entity' is reserved and cannot be a property name";
                    return 0;
                }
                switch (lua_type(L, -1)) {
                    case LUA_TNUMBER: behaviour.defaults.emplace_back(name, lua_tonumber(L, -1)); break;
                    case LUA_TBOOLEAN: behaviour.defaults.emplace_back(name, lua_toboolean(L, -1) != 0); break;
                    case LUA_TSTRING: behaviour.defaults.emplace_back(name, std::string(lua_tostring(L, -1))); break;
                    case LUA_TVECTOR: {
                        const float* v = lua_tovector(L, -1);
                        behaviour.defaults.emplace_back(name, Vec2{v[0], v[1]});
                        break;
                    }
                    default:
                        problem = std::format("property '{}' must default to a number, boolean, string or "
                                              "Vec2 (it is a {})",
                                              name, luaL_typename(L, -1));
                        return 0;
                }
                lua_pop(L, 1);
            }
            std::sort(behaviour.defaults.begin(), behaviour.defaults.end(),
                      [](const auto& a, const auto& b) { return a.first < b.first; });
        } else if (!lua_isnil(L, -1)) {
            problem = std::format("'properties' must be a table (it is a {})", luaL_typename(L, -1));
            return 0;
        }
        lua_pop(L, 1);

        // Shared metatable that makes instances inherit the behaviour's functions.
        lua_createtable(L, 0, 1);
        lua_pushvalue(L, module);
        lua_setfield(L, -2, "__index");
        lua_setreadonly(L, -1, true);
        behaviour.instanceMetatableRef = lua_ref(L, -1);
        return 0;
    });
    if (!result.ok) return makeError(ErrorCode::InvalidData, "{}: {}", script.name, result.message);
    if (!problem.empty()) return makeError(ErrorCode::InvalidData, "{}: {}", script.name, problem);
    for (const auto& w : typoWarnings) m_host.scriptLog(log::Level::Warn, script.name, w);

    m_behaviourIndex.emplace(script.id, m_behaviours.size());
    m_behaviours.push_back(std::move(behaviour));
    return m_behaviours.size() - 1;
}

Status ScriptSystem::createInstance(Entity entity, const scene::AttachedScript& component) {
    const auto* info = m_scene.tryGet<scene::EntityInfo>(entity);
    const std::string entityName = info != nullptr ? info->name : std::string("?");
    const CompiledScript* script = m_library->find(component.script);
    if (script == nullptr) {
        return makeError(ErrorCode::NotFound, "Entity '{}': Scripts refers to {}, which is not a loaded script asset",
                         entityName, component.script.toString());
    }
    auto behaviourIndex = loadBehaviour(*script);
    if (!behaviourIndex) return behaviourIndex.error();
    const Behaviour& behaviour = m_behaviours[behaviourIndex.value()];

    // Validate overrides against the declared properties.
    std::vector<std::string> declared;
    for (const auto& [name, value] : behaviour.defaults) declared.push_back(name);
    for (const auto& prop : component.properties) {
        const auto it = std::find_if(behaviour.defaults.begin(), behaviour.defaults.end(),
                                     [&](const auto& d) { return d.first == prop.name; });
        if (it == behaviour.defaults.end()) {
            return makeError(ErrorCode::InvalidData, "Entity '{}': {} has no property '{}'{}", entityName,
                             script->name, prop.name, suggestName(prop.name, declared));
        }
        if (it->second.index() != prop.value.index()) {
            return makeError(ErrorCode::InvalidData, "Entity '{}': property '{}' of {} must be a {}, not a {}",
                             entityName, prop.name, script->name, valueTypeName(it->second),
                             valueTypeName(prop.value));
        }
    }

    Instance instance;
    instance.entity = entity;
    instance.behaviour = behaviourIndex.value();
    instance.entityName = entityName;
    const ScriptVm::CallResult result = m_vm->runProtected([&](lua_State* L) {
        lua_createtable(L, 0, static_cast<int>(behaviour.defaults.size()) + 1);
        pushEntity(L, entity);
        lua_setfield(L, -2, "entity");
        for (const auto& [name, value] : behaviour.defaults) {
            pushScriptValue(L, value);
            lua_setfield(L, -2, name.c_str());
        }
        for (const auto& prop : component.properties) {
            pushScriptValue(L, prop.value);
            lua_setfield(L, -2, prop.name.c_str());
        }
        lua_getref(L, behaviour.instanceMetatableRef);
        lua_setmetatable(L, -2);
        instance.tableRef = lua_ref(L, -1);
        return 0;
    });
    if (!result.ok) {
        return makeError(ErrorCode::LimitExceeded, "Entity '{}': could not create script instance: {}", entityName,
                         result.message);
    }
    m_byEntity[entity.index].push_back(m_instances.size());
    m_instances.push_back(std::move(instance));
    return {};
}

void ScriptSystem::beginTick() {
    m_logLinesThisTick = 0;
    m_logLimitReported = false;
    // Compact instances of destroyed entities.
    if (std::any_of(m_instances.begin(), m_instances.end(), [](const Instance& i) { return i.destroyed; })) {
        // Listener owners are instance indices; remap them.
        std::vector<int> remap(m_instances.size(), -1);
        std::vector<Instance> kept;
        kept.reserve(m_instances.size());
        for (std::size_t i = 0; i < m_instances.size(); ++i) {
            if (m_instances[i].destroyed) continue;
            remap[i] = static_cast<int>(kept.size());
            kept.push_back(std::move(m_instances[i]));
        }
        m_instances = std::move(kept);
        m_byEntity.clear();
        for (std::size_t i = 0; i < m_instances.size(); ++i) m_byEntity[m_instances[i].entity.index].push_back(i);
        for (auto& l : m_listeners) {
            if (l.ownerInstance >= 0) l.ownerInstance = remap[static_cast<std::size_t>(l.ownerInstance)];
        }
    }
}

std::vector<std::size_t> ScriptSystem::instancesOf(Entity entity) const {
    std::vector<std::size_t> out;
    const auto it = m_byEntity.find(entity.index);
    if (it == m_byEntity.end()) return out;
    for (const std::size_t i : it->second) {
        const Instance& inst = m_instances[i];
        if (!inst.destroyed && inst.entity == entity) out.push_back(i);
    }
    return out;
}

bool ScriptSystem::hasScript(Entity entity) const { return !instancesOf(entity).empty(); }

std::string ScriptSystem::currentScriptName() const {
    if (m_currentInstance >= 0) {
        const Instance& inst = m_instances[static_cast<std::size_t>(m_currentInstance)];
        return m_behaviours[inst.behaviour].script->name;
    }
    if (m_loadingScript != nullptr) return m_loadingScript->name;
    return "script";
}

void ScriptSystem::reportError(const std::string& script, const std::string& entity, const std::string& phase,
                               const ScriptVm::CallResult& result) {
    ++m_errorCount;
    ScriptError error;
    error.script = script;
    error.entity = entity;
    error.phase = phase;
    error.message = result.message;
    error.traceback = result.traceback;
    m_host.scriptError(error);
}

void ScriptSystem::disable(std::size_t index) {
    Instance& inst = m_instances[index];
    inst.enabled = false;
    const int owner = static_cast<int>(index);
    std::erase_if(m_listeners, [&](const Listener& l) {
        if (l.ownerInstance != owner) return false;
        m_vm->releaseRef(l.functionRef);
        return true;
    });
}

void ScriptSystem::call(std::size_t index, LifecycleEvent event, Entity other, float dt) {
    {
        const Instance& inst = m_instances[index];
        if (!inst.enabled || inst.destroyed) return;
    }
    const std::size_t behaviourIndex = m_instances[index].behaviour;
    const int handler = m_behaviours[behaviourIndex].handlers[static_cast<std::size_t>(event)];
    if (handler == 0) return;
    const int tableRef = m_instances[index].tableRef;

    m_currentInstance = static_cast<int>(index);
    const ScriptVm::CallResult result = m_vm->call(handler, [&](lua_State* L) {
        lua_getref(L, tableRef);
        switch (event) {
            case LifecycleEvent::Tick: lua_pushnumber(L, dt); return 2;
            case LifecycleEvent::TriggerEnter:
            case LifecycleEvent::TriggerExit: pushEntity(L, other); return 2;
            default: return 1;
        }
    });
    m_currentInstance = -1;
    if (!result.ok) {
        reportError(m_behaviours[behaviourIndex].script->name, m_instances[index].entityName,
                    kLifecycleEventNames[static_cast<std::size_t>(event)], result);
        disable(index);
    }
}

void ScriptSystem::start() {
    const std::size_t count = m_instances.size();
    for (std::size_t i = 0; i < count; ++i) {
        Instance& inst = m_instances[i];
        if (inst.started || inst.destroyed || !inst.enabled) continue;
        inst.started = true;
        call(i, LifecycleEvent::Start);
    }
}

void ScriptSystem::tick(float dt) {
    const std::size_t count = m_instances.size();
    for (std::size_t i = 0; i < count; ++i) {
        const Instance& inst = m_instances[i];
        if (!inst.started) continue;
        call(i, LifecycleEvent::Tick, scene::kNullEntity, dt);
    }
}

void ScriptSystem::triggerEnter(Entity self, Entity other) {
    for (const std::size_t i : instancesOf(self)) {
        if (m_instances[i].started) call(i, LifecycleEvent::TriggerEnter, other);
    }
}

void ScriptSystem::triggerExit(Entity self, Entity other) {
    for (const std::size_t i : instancesOf(self)) {
        if (m_instances[i].started) call(i, LifecycleEvent::TriggerExit, other);
    }
}

void ScriptSystem::entityDestroyed(Entity entity) {
    const std::vector<std::size_t> instances = instancesOf(entity);
    for (const std::size_t i : instances) {
        if (m_instances[i].started) call(i, LifecycleEvent::Destroy);
    }
    for (const std::size_t i : instances) {
        disable(i);
        m_instances[i].destroyed = true;
        m_vm->releaseRef(m_instances[i].tableRef);
        m_instances[i].tableRef = 0;
    }
    if (!instances.empty()) m_byEntity.erase(entity.index);
    if (const auto it = m_entityHandles.find(entity.index);
        it != m_entityHandles.end() && it->second.generation == entity.generation) {
        m_vm->releaseRef(it->second.ref);
        m_entityHandles.erase(it);
    }
}

bool ScriptSystem::deliverEvents() {
    if (m_eventQueue.empty()) return false;
    const int maxDeliveries = m_options.limits.maxEventDeliveriesPerTick;
    int deliveries = 0;
    for (std::size_t q = 0; q < m_eventQueue.size(); ++q) {
        const QueuedEvent event = m_eventQueue[q]; // copy: handlers may emit more events
        std::string valueText = std::visit(
            [this](const auto& v) -> std::string {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>) return "nil";
                else if constexpr (std::is_same_v<T, double>) return std::format("{}", v);
                else if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
                else if constexpr (std::is_same_v<T, std::string>) return v;
                else if constexpr (std::is_same_v<T, Vec2>) return std::format("({}, {})", v.x, v.y);
                else {
                    const auto* info = m_scene.tryGet<scene::EntityInfo>(v);
                    return "Entity(" + (info ? info->name : std::string("destroyed")) + ")";
                }
            },
            event.value);
        m_host.scriptEvent(event.name, valueText);

        std::vector<int> ids;
        for (const Listener& l : m_listeners) {
            if (l.event == event.name) ids.push_back(l.id);
        }
        for (const int id : ids) {
            const auto it = std::find_if(m_listeners.begin(), m_listeners.end(),
                                         [id](const Listener& l) { return l.id == id; });
            if (it == m_listeners.end()) continue; // removed by an earlier handler
            if (++deliveries > maxDeliveries) {
                ScriptVm::CallResult overflow;
                overflow.ok = false;
                overflow.limitExceeded = true;
                overflow.message = std::format("more than {} event deliveries in one tick; remaining events "
                                               "were dropped (events triggering each other endlessly?)",
                                               maxDeliveries);
                reportError(it->script, "", "event '" + event.name + "'", overflow);
                m_eventQueue.clear();
                return true;
            }
            const int owner = it->ownerInstance;
            const int functionRef = it->functionRef;
            const std::string script = it->script;
            m_currentInstance = owner;
            const ScriptVm::CallResult result = m_vm->call(functionRef, [&](lua_State* L) {
                pushEventValue(L, event.value);
                return 1;
            });
            m_currentInstance = -1;
            if (!result.ok) {
                const std::string entity =
                    owner >= 0 ? m_instances[static_cast<std::size_t>(owner)].entityName : std::string();
                reportError(script, entity, "event '" + event.name + "'", result);
                if (owner >= 0) {
                    disable(static_cast<std::size_t>(owner));
                } else {
                    std::erase_if(m_listeners, [&](const Listener& l) {
                        if (l.id != id) return false;
                        m_vm->releaseRef(l.functionRef);
                        return true;
                    });
                }
            }
        }
    }
    m_eventQueue.clear();
    return true;
}

ScriptStats ScriptSystem::stats() const {
    ScriptStats s;
    s.behaviours = m_behaviours.size();
    for (const Instance& i : m_instances) {
        if (i.destroyed) continue;
        ++s.instances;
        if (!i.enabled) ++s.disabledInstances;
        if (m_behaviours[i.behaviour].handlers[static_cast<std::size_t>(LifecycleEvent::Tick)] != 0) ++s.tickHandlers;
    }
    s.errors = m_errorCount;
    s.memoryBytes = m_vm->memoryUsed();
    return s;
}

} // namespace px::scripting
