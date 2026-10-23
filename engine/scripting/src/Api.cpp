// The gameplay scripting API (version 1).
//
// Every function, property, method and lifecycle event is described once in
// kBindings below. That table registers the Luau functions, decides what
// exists in each realm, and generates docs/scripting-api.md. Add new API
// here, then regenerate the reference (see CONTRIBUTING.md).

#include "Internal.hpp"

#include "px/scripting/ApiReference.hpp"
#include "px/scene/ComponentInfo.hpp"
#include "px/scripting/ScriptSystem.hpp"

#include "px/core/Guid.hpp"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

namespace px::scripting {

namespace {

constexpr int kEntityTag = 1;

struct EntityRef {
    std::uint32_t index;
    std::uint32_t generation;
};

std::size_t editDistance(std::string_view a, std::string_view b) {
    std::vector<std::size_t> prev(b.size() + 1);
    std::vector<std::size_t> cur(b.size() + 1);
    for (std::size_t j = 0; j <= b.size(); ++j) prev[j] = j;
    for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
            const auto lower = [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); };
            const std::size_t cost = lower(a[i - 1]) == lower(b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[b.size()];
}

} // namespace

// " (did you mean 'x'?)" when a candidate is a close spelling of `name`.
std::string suggestName(std::string_view name, const std::vector<std::string>& candidates) {
    std::string best;
    std::size_t bestDistance = 3;
    for (const auto& c : candidates) {
        const std::size_t d = editDistance(name, c);
        if (d < bestDistance) {
            bestDistance = d;
            best = c;
        }
    }
    return best.empty() ? std::string() : std::format(" (did you mean '{}'?)", best);
}

using Getter = void (*)(lua_State*, ScriptSystem&, scene::Entity);
using Setter = void (*)(lua_State*, ScriptSystem&, scene::Entity, int valueIndex);

struct Binding {
    ApiMember info;
    lua_CFunction function = nullptr; // Function, Method, Global
    Getter get = nullptr;             // Property
    Setter set = nullptr;             // Property (writable)
};

struct Bindings {
    static ScriptSystem& system(lua_State* L) { return *static_cast<ScriptSystem*>(ScriptVm::from(L).owner()); }

    // --- argument helpers ---------------------------------------------------------

    static scene::Entity checkEntity(lua_State* L, int idx) {
        auto* ref = static_cast<EntityRef*>(lua_touserdatatagged(L, idx, kEntityTag));
        if (ref == nullptr) luaL_typeerrorL(L, idx, "Entity");
        return scene::Entity{ref->index, ref->generation};
    }

    static scene::Entity checkLiveEntity(lua_State* L, int idx) {
        const scene::Entity e = checkEntity(L, idx);
        if (!system(L).m_scene.isAlive(e)) {
            luaL_errorL(L, "this entity has been destroyed (check with entity:isValid())");
        }
        return e;
    }

    static std::string entityName(ScriptSystem& s, scene::Entity e) {
        const auto* info = s.m_scene.tryGet<scene::EntityInfo>(e);
        return info != nullptr ? info->name : std::string("?");
    }

    template <class T>
    static T& need(lua_State* L, ScriptSystem& s, scene::Entity e, const char* component) {
        T* c = s.m_scene.tryGet<T>(e);
        if (c == nullptr) {
            const std::string name = entityName(s, e);
            luaL_errorL(L, "entity '%s' has no %s component", name.c_str(), component);
        }
        return *c;
    }

    static Vec2 checkVec2(lua_State* L, int idx) {
        const float* v = luaL_checkvector(L, idx);
        if (!std::isfinite(v[0]) || !std::isfinite(v[1])) luaL_argerrorL(L, idx, "vector must be finite");
        return {v[0], v[1]};
    }

    static double checkFinite(lua_State* L, int idx) {
        const double n = luaL_checknumber(L, idx);
        if (!std::isfinite(n)) luaL_argerrorL(L, idx, "number must be finite");
        return n;
    }

    static void requireAuthority(lua_State* L, const char* what) {
        if (system(L).realm() != Realm::Authority) {
            luaL_errorL(L, "%s is only available to authority (server) scripts", what);
        }
    }

    static int checkAction(lua_State* L, int idx) {
        const char* name = luaL_checkstring(L, idx);
        ScriptSystem& s = system(L);
        const int action = s.m_host.findAction(name);
        if (action < 0) {
            const std::string hint = suggestName(name, s.m_host.actionNames());
            luaL_errorL(L, "unknown input action '%s'%s", name, hint.c_str());
        }
        return action;
    }

    // --- globals ------------------------------------------------------------------

    static int vec2(lua_State* L) {
        const double x = checkFinite(L, 1);
        const double y = checkFinite(L, 2);
        lua_pushvector(L, static_cast<float>(x), static_cast<float>(y), 0.0f);
        return 1;
    }

    static int logLine(lua_State* L, log::Level level) {
        ScriptSystem& s = system(L);
        const ScriptLimits& limits = s.m_options.limits;
        if (s.m_logLinesThisTick >= limits.maxLogLinesPerTick) {
            if (!s.m_logLimitReported) {
                s.m_logLimitReported = true;
                s.m_host.scriptLog(log::Level::Warn, s.currentScriptName(),
                                   std::format("more than {} lines printed this tick; further output is dropped",
                                               limits.maxLogLinesPerTick));
            }
            return 0;
        }
        ++s.m_logLinesThisTick;
        std::string text;
        const int n = lua_gettop(L);
        for (int i = 1; i <= n && text.size() < limits.maxLogLineLength; ++i) {
            size_t len = 0;
            const char* part = luaL_tolstring(L, i, &len);
            if (i > 1) text += '\t';
            text.append(part, len);
            lua_pop(L, 1);
        }
        if (text.size() > limits.maxLogLineLength) {
            text.resize(limits.maxLogLineLength);
            text += "...";
        }
        s.m_host.scriptLog(level, s.currentScriptName(), text);
        return 0;
    }
    static int print(lua_State* L) { return logLine(L, log::Level::Info); }
    static int warn(lua_State* L) { return logLine(L, log::Level::Warn); }

    // --- Game -----------------------------------------------------------------------

    static int gameAddScore(lua_State* L) {
        const double amount = checkFinite(L, 1);
        if (amount != std::floor(amount) || std::abs(amount) > 1.0e9) {
            luaL_argerrorL(L, 1, "score amount must be a whole number");
        }
        system(L).m_host.addScore(static_cast<int>(amount));
        return 0;
    }
    static int gameGetScore(lua_State* L) {
        lua_pushinteger(L, system(L).m_host.score());
        return 1;
    }
    static int gameGetObjective(lua_State* L) {
        const std::string text = system(L).m_host.objective();
        lua_pushlstring(L, text.data(), text.size());
        return 1;
    }
    static int gameSetObjective(lua_State* L) {
        size_t len = 0;
        const char* text = luaL_checklstring(L, 1, &len);
        if (len > 200) luaL_argerrorL(L, 1, "objective text is limited to 200 characters");
        system(L).m_host.setObjective(std::string(text, len));
        return 0;
    }
    static int gameCompleteLevel(lua_State* L) {
        size_t len = 0;
        const char* text = luaL_optlstring(L, 1, "Level complete!", &len);
        if (len > 200) luaL_argerrorL(L, 1, "message is limited to 200 characters");
        system(L).m_host.completeLevel(std::string(text, len));
        return 0;
    }
    static int gameTime(lua_State* L) {
        lua_pushnumber(L, system(L).m_host.timeSeconds());
        return 1;
    }
    static int gameTickCount(lua_State* L) {
        lua_pushnumber(L, static_cast<double>(system(L).m_host.tickCount()));
        return 1;
    }

    // --- World ----------------------------------------------------------------------

    static int worldFind(lua_State* L) {
        const char* name = luaL_checkstring(L, 1);
        ScriptSystem& s = system(L);
        const scene::Entity e = s.m_scene.findByName(name);
        if (e.isNull()) {
            lua_pushnil(L);
        } else {
            s.pushEntity(L, e);
        }
        return 1;
    }
    static int worldFindById(lua_State* L) {
        const char* text = luaL_checkstring(L, 1);
        ScriptSystem& s = system(L);
        const auto id = Guid::parse(text);
        const scene::Entity e = id ? s.m_scene.findById(*id) : scene::kNullEntity;
        if (e.isNull()) {
            lua_pushnil(L);
        } else {
            s.pushEntity(L, e);
        }
        return 1;
    }
    static int worldFindWithTag(lua_State* L) {
        const char* tag = luaL_checkstring(L, 1);
        ScriptSystem& s = system(L);
        lua_createtable(L, 0, 0);
        int n = 0;
        for (const scene::Entity e : s.m_scene.entities()) {
            const auto* tags = s.m_scene.tryGet<scene::Tags>(e);
            if (tags == nullptr || !tags->has(tag)) continue;
            s.pushEntity(L, e);
            lua_rawseti(L, -2, ++n);
        }
        return 1;
    }

    // --- Input ----------------------------------------------------------------------

    static int inputIsDown(lua_State* L) {
        const int action = checkAction(L, 1);
        lua_pushboolean(L, system(L).m_host.actionHeld(action));
        return 1;
    }
    static int inputWasPressed(lua_State* L) {
        const int action = checkAction(L, 1);
        lua_pushboolean(L, system(L).m_host.actionPressed(action));
        return 1;
    }

    // --- Events ---------------------------------------------------------------------

    static ScriptSystem::EventValue checkEventValue(lua_State* L, int idx) {
        switch (lua_type(L, idx)) {
            case LUA_TNONE:
            case LUA_TNIL: return std::monostate{};
            case LUA_TNUMBER: return lua_tonumber(L, idx);
            case LUA_TBOOLEAN: return lua_toboolean(L, idx) != 0;
            case LUA_TSTRING: {
                size_t len = 0;
                const char* text = lua_tolstring(L, idx, &len);
                if (len > 1000) luaL_argerrorL(L, idx, "event text is limited to 1000 characters");
                return std::string(text, len);
            }
            case LUA_TVECTOR: return checkVec2(L, idx);
            case LUA_TUSERDATA:
                if (lua_touserdatatagged(L, idx, kEntityTag) != nullptr) return checkEntity(L, idx);
                break;
            default: break;
        }
        luaL_errorL(L, "event values must be nil, a number, boolean, string, vector or Entity (got %s); "
                       "events must be serializable so they can later cross the network",
                    luaL_typename(L, idx));
    }

    static void checkEventName(lua_State* L, int idx, std::string& out) {
        size_t len = 0;
        const char* name = luaL_checklstring(L, idx, &len);
        if (len == 0 || len > 64) luaL_argerrorL(L, idx, "event names must be 1 to 64 characters");
        out.assign(name, len);
    }

    static int eventsEmit(lua_State* L) {
        std::string name;
        checkEventName(L, 1, name);
        ScriptSystem::EventValue value = checkEventValue(L, 2);
        ScriptSystem& s = system(L);
        // Bounds host memory if one callback emits in a loop; deliveries have their own limit.
        const std::size_t maxQueued = 4u * static_cast<std::size_t>(s.m_options.limits.maxEventDeliveriesPerTick);
        if (s.m_eventQueue.size() >= maxQueued) {
            luaL_errorL(L, "too many events queued this tick (limit %d)", static_cast<int>(maxQueued));
        }
        s.m_eventQueue.push_back({std::move(name), std::move(value)});
        return 0;
    }

    static int eventsOn(lua_State* L) {
        std::string name;
        checkEventName(L, 1, name);
        luaL_checktype(L, 2, LUA_TFUNCTION);
        ScriptSystem& s = system(L);
        if (s.m_listeners.size() >= 100000) luaL_errorL(L, "too many event listeners");
        ScriptSystem::Listener listener;
        listener.id = s.m_nextListenerId++;
        listener.event = std::move(name);
        listener.functionRef = lua_ref(L, 2);
        listener.ownerInstance = s.m_currentInstance;
        listener.script = s.currentScriptName();
        s.m_listeners.push_back(std::move(listener));
        lua_pushinteger(L, s.m_listeners.back().id);
        return 1;
    }

    static int eventsOff(lua_State* L) {
        const int id = luaL_checkinteger(L, 1);
        ScriptSystem& s = system(L);
        auto it = std::find_if(s.m_listeners.begin(), s.m_listeners.end(),
                               [id](const ScriptSystem::Listener& l) { return l.id == id; });
        if (it == s.m_listeners.end()) {
            lua_pushboolean(L, false);
            return 1;
        }
        lua_unref(L, it->functionRef);
        s.m_listeners.erase(it);
        lua_pushboolean(L, true);
        return 1;
    }

    // --- Entity properties ------------------------------------------------------------

    static void getId(lua_State* L, ScriptSystem& s, scene::Entity e) {
        const std::string id = need<scene::EntityInfo>(L, s, e, "EntityInfo").id.toString();
        lua_pushlstring(L, id.data(), id.size());
    }
    static void getName(lua_State* L, ScriptSystem& s, scene::Entity e) {
        const std::string& name = need<scene::EntityInfo>(L, s, e, "EntityInfo").name;
        lua_pushlstring(L, name.data(), name.size());
    }
    static void getPosition(lua_State* L, ScriptSystem& s, scene::Entity e) {
        (void)need<scene::Transform>(L, s, e, "Transform");
        const Vec2 p = s.m_scene.worldPosition(e);
        lua_pushvector(L, p.x, p.y, 0.0f);
    }
    static void setPosition(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        (void)need<scene::Transform>(L, s, e, "Transform");
        s.m_scene.setWorldPosition(e, checkVec2(L, idx));
    }
    static void getLocalPosition(lua_State* L, ScriptSystem& s, scene::Entity e) {
        const Vec2 p = need<scene::Transform>(L, s, e, "Transform").position;
        lua_pushvector(L, p.x, p.y, 0.0f);
    }
    static void setLocalPosition(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        need<scene::Transform>(L, s, e, "Transform").position = checkVec2(L, idx);
    }
    static void getRotation(lua_State* L, ScriptSystem& s, scene::Entity e) {
        (void)need<scene::Transform>(L, s, e, "Transform");
        lua_pushnumber(L, s.m_scene.worldTransform(e).rotation);
    }
    static void setRotation(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        (void)need<scene::Transform>(L, s, e, "Transform");
        s.m_scene.setWorldRotation(e, static_cast<float>(checkFinite(L, idx)));
    }
    static void getLocalRotation(lua_State* L, ScriptSystem& s, scene::Entity e) {
        lua_pushnumber(L, need<scene::Transform>(L, s, e, "Transform").rotation);
    }
    static void setLocalRotation(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        need<scene::Transform>(L, s, e, "Transform").rotation = static_cast<float>(checkFinite(L, idx));
    }
    static void getParent(lua_State* L, ScriptSystem& s, scene::Entity e) {
        const scene::Entity parent = s.m_scene.parent(e);
        if (parent.isNull()) {
            lua_pushnil(L);
        } else {
            s.pushEntity(L, parent);
        }
    }
    static void getScale(lua_State* L, ScriptSystem& s, scene::Entity e) {
        const Vec2 v = need<scene::Transform>(L, s, e, "Transform").scale;
        lua_pushvector(L, v.x, v.y, 0.0f);
    }
    static void setScale(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        need<scene::Transform>(L, s, e, "Transform").scale = checkVec2(L, idx);
    }
    static void getVelocity(lua_State* L, ScriptSystem& s, scene::Entity e) {
        const Vec2 v = need<scene::KinematicBody>(L, s, e, "KinematicBody").velocity;
        lua_pushvector(L, v.x, v.y, 0.0f);
    }
    static void setVelocity(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        need<scene::KinematicBody>(L, s, e, "KinematicBody").velocity = checkVec2(L, idx);
    }
    static void getOnGround(lua_State* L, ScriptSystem& s, scene::Entity e) {
        lua_pushboolean(L, need<scene::KinematicBody>(L, s, e, "KinematicBody").onGround);
    }
    static void getVisible(lua_State* L, ScriptSystem& s, scene::Entity e) {
        lua_pushboolean(L, need<scene::Sprite>(L, s, e, "Sprite").visible);
    }
    static void setVisible(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        luaL_checktype(L, idx, LUA_TBOOLEAN);
        need<scene::Sprite>(L, s, e, "Sprite").visible = lua_toboolean(L, idx) != 0;
    }
    static void getSpawnPoint(lua_State* L, ScriptSystem& s, scene::Entity e) {
        const Vec2 v = need<scene::Player>(L, s, e, "Player").spawnPoint;
        lua_pushvector(L, v.x, v.y, 0.0f);
    }
    static void setSpawnPoint(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        need<scene::Player>(L, s, e, "Player").spawnPoint = checkVec2(L, idx);
    }
    static void getFlipX(lua_State* L, ScriptSystem& s, scene::Entity e) {
        lua_pushboolean(L, need<scene::Sprite>(L, s, e, "Sprite").flipX);
    }
    static void setFlipX(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        luaL_checktype(L, idx, LUA_TBOOLEAN);
        need<scene::Sprite>(L, s, e, "Sprite").flipX = lua_toboolean(L, idx) != 0;
    }

    // --- Entity methods ---------------------------------------------------------------

    static int entityIsValid(lua_State* L) {
        lua_pushboolean(L, system(L).m_scene.isAlive(checkEntity(L, 1)));
        return 1;
    }
    static int entityDestroy(lua_State* L) {
        requireAuthority(L, "Entity:destroy");
        const scene::Entity e = checkLiveEntity(L, 1);
        system(L).m_host.requestDestroy(e);
        return 0;
    }
    static int entityRespawn(lua_State* L) {
        requireAuthority(L, "Entity:respawn");
        const scene::Entity e = checkLiveEntity(L, 1);
        if (!system(L).m_host.respawnPlayer(e)) {
            luaL_errorL(L, "respawn: the entity has no Player component (only players respawn)");
        }
        return 0;
    }
    static int entityHasTag(lua_State* L) {
        const scene::Entity e = checkLiveEntity(L, 1);
        const char* tag = luaL_checkstring(L, 2);
        const auto* tags = system(L).m_scene.tryGet<scene::Tags>(e);
        lua_pushboolean(L, tags != nullptr && tags->has(tag));
        return 1;
    }
    static int entityHasComponent(lua_State* L);
    static int entityGetChildren(lua_State* L) {
        const scene::Entity e = checkLiveEntity(L, 1);
        ScriptSystem& s = system(L);
        const std::vector<scene::Entity> children = s.m_scene.children(e);
        lua_createtable(L, static_cast<int>(children.size()), 0);
        int n = 0;
        for (const scene::Entity child : children) {
            s.pushEntity(L, child);
            lua_rawseti(L, -2, ++n);
        }
        return 1;
    }
    static int entitySetParent(lua_State* L) {
        requireAuthority(L, "Entity:setParent");
        const scene::Entity e = checkLiveEntity(L, 1);
        const scene::Entity parent = lua_isnoneornil(L, 2) ? scene::kNullEntity : checkLiveEntity(L, 2);
        const Status status = system(L).m_scene.setParent(e, parent, scene::kNullEntity, true);
        if (!status) luaL_errorL(L, "%s", status.error().message.c_str());
        return 0;
    }

    // --- Entity metatable -------------------------------------------------------------

    static const Binding* findEntityMember(std::string_view name);
    static std::vector<std::string> entityMemberNames();

    static int entityIndex(lua_State* L) {
        const scene::Entity e = checkEntity(L, 1);
        const char* key = luaL_checkstring(L, 2);
        ScriptSystem& s = system(L);
        const Binding* b = findEntityMember(key);
        if (b == nullptr) {
            const std::string hint = suggestName(key, entityMemberNames());
            luaL_errorL(L, "Entity has no member '%s'%s", key, hint.c_str());
        }
        if (b->info.kind == ApiKind::Method) {
            if (b->info.availability == ApiAvailability::AuthorityOnly && s.realm() != Realm::Authority) {
                luaL_errorL(L, "Entity:%s is only available to authority (server) scripts", key);
            }
            lua_getref(L, s.m_entityMethodsRef);
            lua_rawgetfield(L, -1, key);
            return 1;
        }
        if (!s.m_scene.isAlive(e)) luaL_errorL(L, "cannot read '%s': this entity has been destroyed", key);
        b->get(L, s, e);
        return 1;
    }

    static int entityNewIndex(lua_State* L) {
        const scene::Entity e = checkEntity(L, 1);
        const char* key = luaL_checkstring(L, 2);
        ScriptSystem& s = system(L);
        const Binding* b = findEntityMember(key);
        if (b == nullptr) {
            const std::string hint = suggestName(key, entityMemberNames());
            luaL_errorL(L, "Entity has no member '%s'%s (entities cannot hold custom fields; use the script's "
                           "self table)",
                        key, hint.c_str());
        }
        if (b->info.kind != ApiKind::Property || !b->info.writable || b->set == nullptr) {
            luaL_errorL(L, "Entity.%s is read-only", key);
        }
        if (s.realm() != Realm::Authority) {
            luaL_errorL(L, "client scripts cannot change entity state (Entity.%s); the authority decides it", key);
        }
        if (!s.m_scene.isAlive(e)) luaL_errorL(L, "cannot set '%s': this entity has been destroyed", key);
        b->set(L, s, e, 3);
        return 0;
    }

    static int entityEq(lua_State* L) {
        const auto* a = static_cast<EntityRef*>(lua_touserdatatagged(L, 1, kEntityTag));
        const auto* b = static_cast<EntityRef*>(lua_touserdatatagged(L, 2, kEntityTag));
        lua_pushboolean(L, a != nullptr && b != nullptr && a->index == b->index && a->generation == b->generation);
        return 1;
    }

    static int entityToString(lua_State* L) {
        const scene::Entity e = checkEntity(L, 1);
        ScriptSystem& s = system(L);
        if (!s.m_scene.isAlive(e)) {
            lua_pushstring(L, "Entity(destroyed)");
        } else {
            const std::string text = "Entity(" + entityName(s, e) + ")";
            lua_pushlstring(L, text.data(), text.size());
        }
        return 1;
    }

    static void registerApi(lua_State* L, ScriptSystem& s);
};

int Bindings::entityHasComponent(lua_State* L) {
    const scene::Entity e = checkLiveEntity(L, 1);
    std::string name = luaL_checkstring(L, 2);
    if (name == "Script") name = "Scripts"; // scene format 2 name, still accepted
    const scene::ComponentInfo* info = scene::findComponentType(name);
    if (info == nullptr) {
        const std::string hint = suggestName(name, scene::componentTypeNames());
        luaL_errorL(L, "unknown component '%s'%s", name.c_str(), hint.c_str());
    }
    lua_pushboolean(L, info->has(system(L).m_scene, e));
    return 1;
}

namespace {

using K = ApiKind;
using A = ApiAvailability;

// clang-format off
const Binding kBindings[] = {
    // Lifecycle: functions a behaviour table may define.
    {{"Lifecycle", "Start", K::Event, A::Everywhere, "function Behaviour:Start()",
      "Runs once, before the first Tick, after every entity of the scene exists. Set up state here."}},
    {{"Lifecycle", "Tick", K::Event, A::Everywhere, "function Behaviour:Tick(dt: number)",
      "Runs every simulation tick (60 per second by default). dt is the fixed tick length in seconds. Only "
      "behaviours that define Tick cost anything per tick."}},
    {{"Lifecycle", "TriggerEnter", K::Event, A::Everywhere, "function Behaviour:TriggerEnter(other: Entity)",
      "A moving body (KinematicBody) started overlapping a trigger collider. Both entities receive it."}},
    {{"Lifecycle", "TriggerExit", K::Event, A::Everywhere, "function Behaviour:TriggerExit(other: Entity)",
      "The overlap ended, or the other entity was destroyed."}},
    {{"Lifecycle", "Destroy", K::Event, A::Everywhere, "function Behaviour:Destroy()",
      "The entity is about to be removed. It is still readable here. Not called when the whole session ends."}},

    // Globals
    {{"Globals", "Vec2", K::Global, A::Everywhere, "Vec2(x: number, y: number): vector",
      "Creates a 2D vector (a Luau vector with z = 0). Supports + - * / and the vector library.", false, true},
     &Bindings::vec2},
    {{"Globals", "print", K::Global, A::Everywhere, "print(...: any)",
      "Writes a line to the game log, prefixed with the script name. Limited to 100 lines per tick."},
     &Bindings::print},
    {{"Globals", "warn", K::Global, A::Everywhere, "warn(...: any)", "Like print, as a warning."},
     &Bindings::warn},

    // Game: authoritative game state.
    {{"Game", "addScore", K::Function, A::AuthorityOnly, "Game.addScore(amount: number)",
      "Adds a whole number (may be negative) to the score."},
     &Bindings::gameAddScore},
    {{"Game", "getScore", K::Function, A::Everywhere, "Game.getScore(): number", "The current score.", false, true},
     &Bindings::gameGetScore},
    {{"Game", "getObjective", K::Function, A::Everywhere, "Game.getObjective(): string",
      "The objective line currently shown on the HUD (empty if none).", false, true},
     &Bindings::gameGetObjective},
    {{"Game", "setObjective", K::Function, A::AuthorityOnly, "Game.setObjective(text: string)",
      "Sets the objective line shown on the HUD (at most 200 characters)."},
     &Bindings::gameSetObjective},
    {{"Game", "completeLevel", K::Function, A::AuthorityOnly, "Game.completeLevel(message: string?)",
      "Ends the level successfully and shows the message."},
     &Bindings::gameCompleteLevel},
    {{"Game", "time", K::Function, A::Everywhere, "Game.time(): number",
      "Simulated seconds since the session started (deterministic; not the wall clock).", false, true},
     &Bindings::gameTime},
    {{"Game", "tickCount", K::Function, A::Everywhere, "Game.tickCount(): number",
      "Number of ticks simulated so far.", false, true},
     &Bindings::gameTickCount},

    // World: finding entities.
    {{"World", "find", K::Function, A::Everywhere, "World.find(name: string): Entity?",
      "The first entity with this name (in hierarchy order), or nil.", false, true},
     &Bindings::worldFind},
    {{"World", "findById", K::Function, A::Everywhere, "World.findById(id: string): Entity?",
      "The entity with this id, or nil.", false, true},
     &Bindings::worldFindById},
    {{"World", "findWithTag", K::Function, A::Everywhere, "World.findWithTag(tag: string): {Entity}",
      "All entities with the tag, in hierarchy order.", false, true},
     &Bindings::worldFindWithTag},

    // Input: named actions from the project's input settings.
    {{"Input", "isDown", K::Function, A::Everywhere, "Input.isDown(action: string): boolean",
      "True while the action is held this tick. Unknown action names are errors.", false, true},
     &Bindings::inputIsDown},
    {{"Input", "wasPressed", K::Function, A::Everywhere, "Input.wasPressed(action: string): boolean",
      "True on the tick the action went down.", false, true},
     &Bindings::inputWasPressed},

    // Events: messages between scripts.
    {{"Events", "emit", K::Function, A::Everywhere, "Events.emit(name: string, value: any?)",
      "Queues an event. Listeners run after the current phase, in registration order. value must be nil, "
      "number, boolean, string, vector or Entity."},
     &Bindings::eventsEmit},
    {{"Events", "on", K::Function, A::Everywhere, "Events.on(name: string, handler: (value: any?) -> ()): number",
      "Registers a listener and returns its id. Listeners registered by a behaviour are removed when its "
      "entity is destroyed or its script fails."},
     &Bindings::eventsOn},
    {{"Events", "off", K::Function, A::Everywhere, "Events.off(id: number): boolean",
      "Removes a listener. Returns false if it did not exist."},
     &Bindings::eventsOff},

    // Entity: handles to scene entities.
    {{"Entity", "id", K::Property, A::Everywhere, "entity.id: string", "Stable unique id (from the scene file).",
      false, true}, nullptr, &Bindings::getId},
    {{"Entity", "name", K::Property, A::Everywhere, "entity.name: string", "Display name.", false, true},
     nullptr, &Bindings::getName},
    {{"Entity", "position", K::Property, A::Everywhere, "entity.position: vector",
      "World position (Transform). Y points down. Assigning teleports the entity; its children move with it.",
      true, true},
     nullptr, &Bindings::getPosition, &Bindings::setPosition},
    {{"Entity", "localPosition", K::Property, A::Everywhere, "entity.localPosition: vector",
      "Position relative to the parent (the Transform's own value). Same as position for entities without a "
      "parent.", true, true},
     nullptr, &Bindings::getLocalPosition, &Bindings::setLocalPosition},
    {{"Entity", "rotation", K::Property, A::Everywhere, "entity.rotation: number",
      "World rotation in degrees, clockwise (Transform).", true, true},
     nullptr, &Bindings::getRotation, &Bindings::setRotation},
    {{"Entity", "localRotation", K::Property, A::Everywhere, "entity.localRotation: number",
      "Rotation relative to the parent, in degrees.", true, true},
     nullptr, &Bindings::getLocalRotation, &Bindings::setLocalRotation},
    {{"Entity", "scale", K::Property, A::Everywhere, "entity.scale: vector",
      "Scale relative to the parent (Transform). Negative values mirror.", true, true},
     nullptr, &Bindings::getScale, &Bindings::setScale},
    {{"Entity", "parent", K::Property, A::Everywhere, "entity.parent: Entity?",
      "The parent entity, or nil for top-level entities. Change it with setParent.", false, true},
     nullptr, &Bindings::getParent},
    {{"Entity", "velocity", K::Property, A::Everywhere, "entity.velocity: vector",
      "Velocity in units per second (KinematicBody).", true, true},
     nullptr, &Bindings::getVelocity, &Bindings::setVelocity},
    {{"Entity", "onGround", K::Property, A::Everywhere, "entity.onGround: boolean",
      "True if the body is standing on something (KinematicBody).", false, true},
     nullptr, &Bindings::getOnGround},
    {{"Entity", "visible", K::Property, A::Everywhere, "entity.visible: boolean", "Whether the Sprite is drawn.",
      true, true}, nullptr, &Bindings::getVisible, &Bindings::setVisible},
    {{"Entity", "flipX", K::Property, A::Everywhere, "entity.flipX: boolean", "Mirrors the Sprite horizontally.",
      true, true}, nullptr, &Bindings::getFlipX, &Bindings::setFlipX},
    {{"Entity", "spawnPoint", K::Property, A::Everywhere, "entity.spawnPoint: vector",
      "Where a Player respawns (its starting position unless changed). Set it in a checkpoint to make the player "
      "come back there.", true, true},
     nullptr, &Bindings::getSpawnPoint, &Bindings::setSpawnPoint},
    {{"Entity", "isValid", K::Method, A::Everywhere, "entity:isValid(): boolean",
      "False once the entity has been destroyed. Other members raise an error on destroyed entities.", false,
      true}, &Bindings::entityIsValid},
    {{"Entity", "destroy", K::Method, A::AuthorityOnly, "entity:destroy()",
      "Removes the entity and its children at the end of the current phase (after the running handlers)."},
     &Bindings::entityDestroy},
    {{"Entity", "respawn", K::Method, A::AuthorityOnly, "entity:respawn()",
      "Sends a Player back to its spawn point and stops it, as when it falls off the level (counts as a "
      "respawn). Use it for hazards and enemies. Errors for entities without a Player component."},
     &Bindings::entityRespawn},
    {{"Entity", "hasTag", K::Method, A::Everywhere, "entity:hasTag(tag: string): boolean",
      "True if the entity's Tags component contains the tag.", false, true},
     &Bindings::entityHasTag},
    {{"Entity", "hasComponent", K::Method, A::Everywhere, "entity:hasComponent(name: string): boolean",
      "True if the entity has the named component, e.g. \"Player\" (see docs/components.md).", false, true},
     &Bindings::entityHasComponent},
    {{"Entity", "getChildren", K::Method, A::Everywhere, "entity:getChildren(): {Entity}",
      "The entity's direct children, in hierarchy order.", false, true},
     &Bindings::entityGetChildren},
    {{"Entity", "setParent", K::Method, A::AuthorityOnly, "entity:setParent(parent: Entity?)",
      "Moves the entity (with its children) under another entity, or to the top level with nil. It stays where "
      "it is in the world. Errors if the parent is the entity itself or one of its descendants."},
     &Bindings::entitySetParent},
};
// clang-format on

} // namespace

const Binding* Bindings::findEntityMember(std::string_view name) {
    static const std::unordered_map<std::string_view, const Binding*> members = [] {
        std::unordered_map<std::string_view, const Binding*> m;
        for (const Binding& b : kBindings) {
            if (b.info.group == "Entity") m.emplace(b.info.name, &b);
        }
        return m;
    }();
    const auto it = members.find(name);
    return it == members.end() ? nullptr : it->second;
}

std::vector<std::string> Bindings::entityMemberNames() {
    std::vector<std::string> names;
    for (const Binding& b : kBindings) {
        if (b.info.group == "Entity") names.emplace_back(b.info.name);
    }
    return names;
}

void Bindings::registerApi(lua_State* L, ScriptSystem& s) {
    const bool authority = s.realm() == Realm::Authority;
    const auto available = [authority](const Binding& b) {
        return authority || b.info.availability == ApiAvailability::Everywhere;
    };

    // Globals and library tables.
    for (const char* group : {"Game", "World", "Input", "Events"}) {
        lua_createtable(L, 0, 8);
        for (const Binding& b : kBindings) {
            if (b.info.group != group || b.function == nullptr || !available(b)) continue;
            const std::string name(b.info.name);
            const std::string debugName = std::string(group) + "." + name;
            lua_pushcfunction(L, b.function, debugName.c_str());
            lua_setfield(L, -2, name.c_str());
        }
        lua_setglobal(L, group);
    }
    for (const Binding& b : kBindings) {
        if (b.info.kind != ApiKind::Global || !available(b)) continue;
        const std::string name(b.info.name);
        lua_pushcfunction(L, b.function, name.c_str());
        lua_setglobal(L, name.c_str());
    }

    // Entity methods table (read-only, referenced by __index).
    lua_createtable(L, 0, 8);
    for (const Binding& b : kBindings) {
        if (b.info.group != "Entity" || b.info.kind != ApiKind::Method || !available(b)) continue;
        const std::string name(b.info.name);
        lua_pushcfunction(L, b.function, name.c_str());
        lua_setfield(L, -2, name.c_str());
    }
    lua_setreadonly(L, -1, true);
    s.m_entityMethodsRef = lua_ref(L, -1);
    lua_pop(L, 1);

    // Entity metatable: dispatches properties and methods, locked against scripts.
    lua_createtable(L, 0, 6);
    lua_pushcfunction(L, &Bindings::entityIndex, "Entity.__index");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, &Bindings::entityNewIndex, "Entity.__newindex");
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, &Bindings::entityEq, "Entity.__eq");
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, &Bindings::entityToString, "Entity.__tostring");
    lua_setfield(L, -2, "__tostring");
    lua_pushstring(L, "Entity");
    lua_setfield(L, -2, "__type");
    lua_pushstring(L, "Entity"); // getmetatable() returns this string; setmetatable() is refused
    lua_setfield(L, -2, "__metatable");
    lua_setreadonly(L, -1, true);
    lua_setuserdatametatable(L, kEntityTag);
}

void ScriptSystem::pushEntity(lua_State* L, scene::Entity entity) {
    auto it = m_entityHandles.find(entity.index);
    if (it != m_entityHandles.end()) {
        if (it->second.generation == entity.generation) {
            lua_getref(L, it->second.ref);
            return;
        }
        lua_unref(L, it->second.ref);
        m_entityHandles.erase(it);
    }
    auto* ref = static_cast<EntityRef*>(lua_newuserdatataggedwithmetatable(L, sizeof(EntityRef), kEntityTag));
    ref->index = entity.index;
    ref->generation = entity.generation;
    const int handle = lua_ref(L, -1);
    m_entityHandles[entity.index] = EntityHandle{entity.generation, handle};
}

void ScriptSystem::pushEventValue(lua_State* L, const EventValue& value) {
    std::visit(
        [this, L](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                lua_pushnil(L);
            } else if constexpr (std::is_same_v<T, double>) {
                lua_pushnumber(L, v);
            } else if constexpr (std::is_same_v<T, bool>) {
                lua_pushboolean(L, v);
            } else if constexpr (std::is_same_v<T, std::string>) {
                lua_pushlstring(L, v.data(), v.size());
            } else if constexpr (std::is_same_v<T, Vec2>) {
                lua_pushvector(L, v.x, v.y, 0.0f);
            } else {
                if (m_scene.isAlive(v)) {
                    pushEntity(L, v);
                } else {
                    lua_pushnil(L); // the entity was destroyed before delivery
                }
            }
        },
        value);
}

void registerScriptApi(lua_State* L, ScriptSystem& system) { Bindings::registerApi(L, system); }

// --- Reference -------------------------------------------------------------------------

const char* apiKindName(ApiKind kind) noexcept {
    switch (kind) {
        case ApiKind::Function: return "function";
        case ApiKind::Method: return "method";
        case ApiKind::Property: return "property";
        case ApiKind::Event: return "event";
        case ApiKind::Global: return "global";
    }
    return "?";
}

std::span<const ApiMember> apiReference() {
    static const std::vector<ApiMember> members = [] {
        std::vector<ApiMember> out;
        for (const Binding& b : kBindings) out.push_back(b.info);
        return out;
    }();
    return members;
}

std::string generateApiReferenceMarkdown() {
    std::string md;
    md += "# Scripting API reference (version 1)\n\n";
    md += "<!-- Generated from engine/scripting/src/Api.cpp by `px-headless --print-api-reference`.\n"
          "     Do not edit by hand: the test \"API reference document is up to date\" fails if this\n"
          "     file differs from the generated text. -->\n\n";
    md += "Guide and examples: [scripting.md](scripting.md). *Available in*: **all** means authority and "
          "client scripts; **authority** means only where game state is decided (single-player session, "
          "server). *Pure* members have no side effects (data nodes in visual scripting).\n";
    const char* groups[] = {"Lifecycle", "Globals", "Game", "World", "Input", "Events", "Entity"};
    const char* intro[] = {
        "Functions a behaviour table may define. The engine calls them; see the lifecycle rules in scripting.md.",
        "Global functions.",
        "Game state. Changes are authoritative and, in multiplayer, will be replicated to clients.",
        "Finding entities in the running scene.",
        "Input actions defined in the project's `input.actions`.",
        "Custom events between scripts. Values must be serializable.",
        "An `Entity` is a handle to a scene entity. Properties need the named component; accessing a "
        "destroyed entity is an error except `isValid()`. Writable properties can only be assigned by "
        "authority scripts.",
    };
    for (std::size_t g = 0; g < std::size(groups); ++g) {
        md += std::format("\n## {}\n\n{}\n\n", groups[g], intro[g]);
        md += "| Member | Kind | Available in | Description |\n|---|---|---|---|\n";
        for (const Binding& b : kBindings) {
            if (b.info.group != groups[g]) continue;
            std::string kind = apiKindName(b.info.kind);
            if (b.info.kind == ApiKind::Property) kind += b.info.writable ? " (read/write)" : " (read-only)";
            if (b.info.pure) kind += ", pure";
            md += std::format("| `{}` | {} | {} | {} |\n", b.info.signature, kind,
                              b.info.availability == ApiAvailability::Everywhere ? "all" : "authority",
                              b.info.description);
        }
    }
    return md;
}

} // namespace px::scripting
