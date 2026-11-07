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
#include <optional>
#include <variant>
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

    static int worldSpawn(lua_State* L) {
        requireAuthority(L, "World.spawn");
        size_t length = 0;
        const char* name = luaL_checklstring(L, 1, &length);
        ScriptSystem& s = system(L);
        const scene::Entity parent = lua_isnoneornil(L, 3) ? scene::kNullEntity : checkLiveEntity(L, 3);
        Vec2 position{};
        if (!lua_isnoneornil(L, 2)) {
            position = checkVec2(L, 2);
        } else if (!parent.isNull()) {
            position = s.m_scene.worldPosition(parent);
        }
        auto spawned = s.m_host.spawnPrefab(std::string_view(name, length), position, parent);
        if (!spawned) luaL_errorL(L, "World.spawn: %s", spawned.error().message.c_str());
        s.pushEntity(L, spawned.value());
        return 1;
    }

    // --- Sound: sounds and music (docs/audio.md) --------------------------------------

    static assets::AssetId checkSound(lua_State* L, int idx, const char* what) {
        size_t length = 0;
        const char* name = luaL_checklstring(L, idx, &length);
        ScriptSystem& s = system(L);
        const assets::AssetId id = s.m_host.findSound(std::string_view(name, length));
        if (id.isNil()) {
            const std::string hint = suggestName(name, s.m_host.soundNames());
            luaL_errorL(L, "%s: no sound named '%s'%s", what, name, hint.c_str());
        }
        return id;
    }
    static float optionalLevel(lua_State* L, int idx, double fallback, double lo, double hi) {
        if (lua_isnoneornil(L, idx)) return static_cast<float>(fallback);
        return static_cast<float>(std::clamp(checkFinite(L, idx), lo, hi));
    }
    static void play(lua_State* L, const char* what, SoundRequest request) {
        if (auto played = system(L).m_host.playSound(request); !played) {
            luaL_errorL(L, "%s: %s", what, played.error().message.c_str());
        }
    }
    static int soundPlay(lua_State* L) {
        requireAuthority(L, "Sound.play");
        SoundRequest r;
        r.sound = checkSound(L, 1, "Sound.play");
        r.volume = optionalLevel(L, 2, 1.0, 0.0, 4.0);
        r.pitch = optionalLevel(L, 3, 1.0, 0.25, 4.0);
        play(L, "Sound.play", r);
        return 0;
    }
    static int soundPlayAt(lua_State* L) {
        requireAuthority(L, "Sound.playAt");
        SoundRequest r;
        r.sound = checkSound(L, 1, "Sound.playAt");
        r.position = checkVec2(L, 2);
        r.volume = optionalLevel(L, 3, 1.0, 0.0, 4.0);
        r.pitch = optionalLevel(L, 4, 1.0, 0.25, 4.0);
        play(L, "Sound.playAt", r);
        return 0;
    }
    static int soundPlayFor(lua_State* L) {
        requireAuthority(L, "Sound.playFor");
        SoundRequest r;
        r.listener = checkLiveEntity(L, 1);
        r.sound = checkSound(L, 2, "Sound.playFor");
        r.volume = optionalLevel(L, 3, 1.0, 0.0, 4.0);
        r.pitch = optionalLevel(L, 4, 1.0, 0.25, 4.0);
        play(L, "Sound.playFor", r);
        return 0;
    }
    static int entityPlaySound(lua_State* L) {
        requireAuthority(L, "Entity:playSound");
        SoundRequest r;
        r.at = checkLiveEntity(L, 1);
        r.sound = checkSound(L, 2, "playSound");
        r.volume = optionalLevel(L, 3, 1.0, 0.0, 4.0);
        r.pitch = optionalLevel(L, 4, 1.0, 0.25, 4.0);
        play(L, "playSound", r);
        return 0;
    }
    static void music(lua_State* L, const char* what, scene::Entity player, int nameIdx) {
        const assets::AssetId sound = lua_isnoneornil(L, nameIdx) ? assets::AssetId{} : checkSound(L, nameIdx, what);
        const float volume = optionalLevel(L, nameIdx + 1, 1.0, 0.0, 4.0);
        const float fade = optionalLevel(L, nameIdx + 2, 1.0, 0.0, 60.0);
        if (auto ok = system(L).m_host.playMusic(player, sound, volume, fade); !ok) {
            luaL_errorL(L, "%s: %s", what, ok.error().message.c_str());
        }
    }
    static int soundPlayMusic(lua_State* L) {
        requireAuthority(L, "Sound.playMusic");
        (void)luaL_checkstring(L, 1);
        music(L, "Sound.playMusic", scene::kNullEntity, 1);
        return 0;
    }
    static int soundPlayMusicFor(lua_State* L) {
        requireAuthority(L, "Sound.playMusicFor");
        music(L, "Sound.playMusicFor", checkLiveEntity(L, 1), 2);
        return 0;
    }
    static int soundStopMusic(lua_State* L) {
        requireAuthority(L, "Sound.stopMusic");
        const float fade = optionalLevel(L, 1, 1.0, 0.0, 60.0);
        if (auto ok = system(L).m_host.playMusic(scene::kNullEntity, {}, 1.0f, fade); !ok) {
            luaL_errorL(L, "Sound.stopMusic: %s", ok.error().message.c_str());
        }
        return 0;
    }
    static int soundMusic(lua_State* L) {
        ScriptSystem& s = system(L);
        const std::string name = s.m_host.soundName(s.m_host.music());
        lua_pushlstring(L, name.data(), name.size());
        return 1;
    }

    // The tile of `map` (on entity e) at world point p, if there is one.
    static std::optional<int> tileAt(const scene::Scene& scene, scene::Entity e, const scene::Tilemap& map, Vec2 p) {
        if (map.tileSize.x <= 0.0f || map.tileSize.y <= 0.0f) return std::nullopt;
        const Vec2 origin = scene.worldPosition(e);
        const float fx = (p.x - origin.x) / map.tileSize.x;
        const float fy = (p.y - origin.y) / map.tileSize.y;
        if (!(fx >= 0.0f) || !(fy >= 0.0f) || fx >= static_cast<float>(map.width) || fy >= static_cast<float>(map.height)) {
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(fy) * static_cast<std::size_t>(map.width) + static_cast<std::size_t>(fx);
        if (index >= map.tiles.size() || map.tiles[index] < 0) return std::nullopt;
        return map.tiles[index];
    }
    static int worldTileAt(lua_State* L) {
        const Vec2 p = checkVec2(L, 1);
        ScriptSystem& s = system(L);
        std::optional<int> best;
        int bestLayer = 0;
        for (const scene::Entity e : s.m_scene.entities()) {
            const auto* map = s.m_scene.tryGet<scene::Tilemap>(e);
            if (map == nullptr || s.m_scene.tryGet<scene::Transform>(e) == nullptr) continue;
            const auto tile = tileAt(s.m_scene, e, *map, p);
            if (tile && (!best || map->layer >= bestLayer)) {
                best = tile;
                bestLayer = map->layer;
            }
        }
        if (best) {
            lua_pushnumber(L, *best);
        } else {
            lua_pushnil(L);
        }
        return 1;
    }
    static int worldTileHasTag(lua_State* L) {
        const Vec2 p = checkVec2(L, 1);
        const char* tag = luaL_checkstring(L, 2);
        ScriptSystem& s = system(L);
        for (const scene::Entity e : s.m_scene.entities()) {
            const auto* map = s.m_scene.tryGet<scene::Tilemap>(e);
            if (map == nullptr || s.m_scene.tryGet<scene::Transform>(e) == nullptr) continue;
            const auto tile = tileAt(s.m_scene, e, *map, p);
            if (!tile) continue;
            const auto* tags = s.m_host.tileTags(map->tileset, map->tileSize, *tile);
            if (tags != nullptr && std::find(tags->begin(), tags->end(), tag) != tags->end()) {
                lua_pushboolean(L, 1);
                return 1;
            }
        }
        lua_pushboolean(L, 0);
        return 1;
    }

    // --- Input ----------------------------------------------------------------------

    // A player character: a live entity with a Player component.
    static scene::Entity checkPlayer(lua_State* L, int idx) {
        const scene::Entity e = checkLiveEntity(L, idx);
        if (system(L).m_scene.tryGet<scene::Player>(e) == nullptr) {
            luaL_argerrorL(L, idx, "expected a player character (an entity with a Player component)");
        }
        return e;
    }

    static int inputIsDown(lua_State* L) {
        const int action = checkAction(L, 1);
        ScriptSystem& s = system(L);
        if (lua_isnoneornil(L, 2)) {
            lua_pushboolean(L, s.m_host.actionHeld(action));
        } else {
            lua_pushboolean(L, s.m_host.actionHeldBy(checkPlayer(L, 2), action));
        }
        return 1;
    }
    static int inputWasPressed(lua_State* L) {
        const int action = checkAction(L, 1);
        ScriptSystem& s = system(L);
        if (lua_isnoneornil(L, 2)) {
            lua_pushboolean(L, s.m_host.actionPressed(action));
        } else {
            lua_pushboolean(L, s.m_host.actionPressedBy(checkPlayer(L, 2), action));
        }
        return 1;
    }

    // --- Players --------------------------------------------------------------------

    static int playersGetAll(lua_State* L) {
        ScriptSystem& s = system(L);
        lua_createtable(L, 0, 0);
        int n = 0;
        for (const scene::Entity e : s.m_scene.entities()) {
            if (s.m_scene.tryGet<scene::Player>(e) == nullptr) continue;
            s.pushEntity(L, e);
            lua_rawseti(L, -2, ++n);
        }
        return 1;
    }
    static int playersCount(lua_State* L) {
        lua_pushnumber(L, static_cast<double>(system(L).m_scene.registry().count<scene::Player>()));
        return 1;
    }

    // --- Data: saved player data -------------------------------------------------------

    static scene::ScriptValue checkDataValue(lua_State* L, int idx) {
        switch (lua_type(L, idx)) {
            case LUA_TNUMBER: return checkFinite(L, idx);
            case LUA_TBOOLEAN: return lua_toboolean(L, idx) != 0;
            case LUA_TSTRING: {
                size_t len = 0;
                const char* text = lua_tolstring(L, idx, &len);
                return std::string(text, len);
            }
            case LUA_TVECTOR: return checkVec2(L, idx);
            default: break;
        }
        luaL_errorL(L, "saved values must be a number, boolean, string or vector (got %s)", luaL_typename(L, idx));
    }

    static void pushDataValue(lua_State* L, const scene::ScriptValue& value) {
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

    static std::string checkDataKey(lua_State* L, int idx) {
        size_t len = 0;
        const char* key = luaL_checklstring(L, idx, &len);
        return std::string(key, len);
    }

    static int dataGet(lua_State* L) {
        const scene::Entity player = checkEntity(L, 1);
        const std::string key = checkDataKey(L, 2);
        auto value = system(L).m_host.playerDataGet(player, key);
        if (!value) luaL_errorL(L, "Data.get: %s", value.error().message.c_str());
        if (!value.value()) {
            lua_pushnil(L);
        } else {
            pushDataValue(L, *value.value());
        }
        return 1;
    }

    static int dataSet(lua_State* L) {
        const scene::Entity player = checkEntity(L, 1);
        const std::string key = checkDataKey(L, 2);
        std::optional<scene::ScriptValue> value;
        if (!lua_isnoneornil(L, 3)) value = checkDataValue(L, 3);
        auto done = system(L).m_host.playerDataSet(player, key, std::move(value));
        if (!done) luaL_errorL(L, "Data.set: %s", done.error().message.c_str());
        return 0;
    }

    static int dataIncrement(lua_State* L) {
        const scene::Entity player = checkEntity(L, 1);
        const std::string key = checkDataKey(L, 2);
        const double amount = lua_isnoneornil(L, 3) ? 1.0 : checkFinite(L, 3);
        IScriptHost& host = system(L).m_host;
        auto current = host.playerDataGet(player, key);
        if (!current) luaL_errorL(L, "Data.increment: %s", current.error().message.c_str());
        double base = 0.0;
        if (current.value()) {
            const auto* number = std::get_if<double>(&*current.value());
            if (number == nullptr) luaL_errorL(L, "Data.increment: '%s' does not hold a number", key.c_str());
            base = *number;
        }
        const double next = base + amount;
        if (!std::isfinite(next)) luaL_errorL(L, "Data.increment: the result is not a finite number");
        auto done = host.playerDataSet(player, key, scene::ScriptValue(next));
        if (!done) luaL_errorL(L, "Data.increment: %s", done.error().message.c_str());
        lua_pushnumber(L, next);
        return 1;
    }

    // --- Leaderboards ---------------------------------------------------------------

    static std::string checkBoard(lua_State* L, int idx) {
        size_t len = 0;
        const char* board = luaL_checklstring(L, idx, &len);
        return std::string(board, len);
    }

    static void pushRow(lua_State* L, const LeaderboardRow& row) {
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, row.rank);
        lua_setfield(L, -2, "rank");
        lua_pushlstring(L, row.name.data(), row.name.size());
        lua_setfield(L, -2, "name");
        lua_pushnumber(L, row.score);
        lua_setfield(L, -2, "score");
    }

    static int leaderboardSubmit(lua_State* L) {
        const scene::Entity player = checkEntity(L, 1);
        const std::string board = checkBoard(L, 2);
        const double score = checkFinite(L, 3);
        auto best = system(L).m_host.leaderboardSubmit(player, board, score);
        if (!best) luaL_errorL(L, "Leaderboard.submit: %s", best.error().message.c_str());
        lua_pushboolean(L, best.value() ? 1 : 0);
        return 1;
    }

    static int leaderboardBest(lua_State* L) {
        const scene::Entity player = checkEntity(L, 1);
        const std::string board = checkBoard(L, 2);
        auto entry = system(L).m_host.leaderboardEntry(player, board);
        if (!entry) luaL_errorL(L, "Leaderboard.best: %s", entry.error().message.c_str());
        if (entry.value()) {
            lua_pushnumber(L, entry.value()->score);
        } else {
            lua_pushnil(L);
        }
        return 1;
    }

    static int leaderboardRank(lua_State* L) {
        const scene::Entity player = checkEntity(L, 1);
        const std::string board = checkBoard(L, 2);
        auto entry = system(L).m_host.leaderboardEntry(player, board);
        if (!entry) luaL_errorL(L, "Leaderboard.rank: %s", entry.error().message.c_str());
        if (entry.value()) {
            lua_pushinteger(L, entry.value()->rank);
        } else {
            lua_pushnil(L);
        }
        return 1;
    }

    static int leaderboardTop(lua_State* L) {
        const std::string board = checkBoard(L, 1);
        const double count = lua_isnoneornil(L, 2) ? 10.0 : checkFinite(L, 2);
        if (count < 1 || count > 100) luaL_argerrorL(L, 2, "count must be 1 to 100");
        auto rows = system(L).m_host.leaderboardTop(board, static_cast<std::size_t>(count));
        if (!rows) luaL_errorL(L, "Leaderboard.top: %s", rows.error().message.c_str());
        lua_createtable(L, static_cast<int>(rows.value().size()), 0);
        for (std::size_t i = 0; i < rows.value().size(); ++i) {
            pushRow(L, rows.value()[i]);
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
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
        if (const auto* text = s.m_scene.tryGet<scene::Text>(e); text != nullptr && s.m_scene.tryGet<scene::Sprite>(e) == nullptr) {
            lua_pushboolean(L, text->visible);
            return;
        }
        lua_pushboolean(L, need<scene::Sprite>(L, s, e, "Sprite").visible);
    }
    static void setVisible(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        luaL_checktype(L, idx, LUA_TBOOLEAN);
        const bool visible = lua_toboolean(L, idx) != 0;
        auto* sprite = s.m_scene.tryGet<scene::Sprite>(e);
        auto* text = s.m_scene.tryGet<scene::Text>(e);
        if (sprite == nullptr && text == nullptr) (void)need<scene::Sprite>(L, s, e, "Sprite or Text");
        if (sprite != nullptr) sprite->visible = visible;
        if (text != nullptr) text->visible = visible;
    }
    static void getText(lua_State* L, ScriptSystem& s, scene::Entity e) {
        const std::string& text = need<scene::Text>(L, s, e, "Text").text;
        lua_pushlstring(L, text.data(), text.size());
    }
    static void setText(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        size_t length = 0;
        const char* text = luaL_checklstring(L, idx, &length);
        if (length > scene::kMaxTextLength) {
            luaL_errorL(L, "entity.text: at most %d bytes", static_cast<int>(scene::kMaxTextLength));
        }
        need<scene::Text>(L, s, e, "Text").text.assign(text, length);
    }
    static void getTextColor(lua_State* L, ScriptSystem& s, scene::Entity e) {
        const Color c = need<scene::Text>(L, s, e, "Text").color;
        const std::string hex = std::format("#{:02x}{:02x}{:02x}{:02x}", c.r, c.g, c.b, c.a);
        lua_pushlstring(L, hex.data(), hex.size());
    }
    static void setTextColor(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        const std::string hex = luaL_checkstring(L, idx);
        const auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        const auto byteAt = [&](std::size_t i) { return digit(hex[i]) * 16 + digit(hex[i + 1]); };
        bool ok = (hex.size() == 7 || hex.size() == 9) && hex[0] == '#';
        for (std::size_t i = 1; ok && i < hex.size(); ++i) ok = digit(hex[i]) >= 0;
        if (!ok) luaL_errorL(L, "entity.textColor: expected \"#rrggbb\" or \"#rrggbbaa\" (got \"%s\")", hex.c_str());
        Color c{static_cast<std::uint8_t>(byteAt(1)), static_cast<std::uint8_t>(byteAt(3)),
                static_cast<std::uint8_t>(byteAt(5)), static_cast<std::uint8_t>(hex.size() == 9 ? byteAt(7) : 255)};
        need<scene::Text>(L, s, e, "Text").color = c;
    }
    static void getSoundPlaying(lua_State* L, ScriptSystem& s, scene::Entity e) {
        lua_pushboolean(L, need<scene::AudioSource>(L, s, e, "AudioSource").playing);
    }
    static void setSoundPlaying(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        luaL_checktype(L, idx, LUA_TBOOLEAN);
        need<scene::AudioSource>(L, s, e, "AudioSource").playing = lua_toboolean(L, idx) != 0;
    }
    static void getSpawnPoint(lua_State* L, ScriptSystem& s, scene::Entity e) {
        const Vec2 v = need<scene::Player>(L, s, e, "Player").spawnPoint;
        lua_pushvector(L, v.x, v.y, 0.0f);
    }
    static void setSpawnPoint(lua_State* L, ScriptSystem& s, scene::Entity e, int idx) {
        need<scene::Player>(L, s, e, "Player").spawnPoint = checkVec2(L, idx);
    }
    static void getPlayerName(lua_State* L, ScriptSystem& s, scene::Entity e) {
        const std::string& name = need<scene::Player>(L, s, e, "Player").name;
        // Playing alone there is no player name; the character's name stands in.
        const std::string& shown = name.empty() ? need<scene::EntityInfo>(L, s, e, "EntityInfo").name : name;
        lua_pushlstring(L, shown.data(), shown.size());
    }
    static void getAnimation(lua_State* L, ScriptSystem& s, scene::Entity e) {
        const std::string name = s.m_host.animationName(need<scene::Animator>(L, s, e, "Animator").current);
        lua_pushlstring(L, name.data(), name.size());
    }
    static int entityPlayAnimation(lua_State* L) {
        requireAuthority(L, "Entity:playAnimation");
        const scene::Entity e = checkLiveEntity(L, 1);
        ScriptSystem& s = system(L);
        scene::Animator& animator = need<scene::Animator>(L, s, e, "Animator");
        if (lua_isnoneornil(L, 2)) {
            animator.scripted = {}; // back to the automatic choice
            return 0;
        }
        const char* name = luaL_checkstring(L, 2);
        const assets::AssetId clip = s.m_host.findAnimation(name);
        if (clip.isNil()) {
            const std::string hint = suggestName(name, s.m_host.animationNames());
            luaL_errorL(L, "playAnimation: no animation named '%s'%s", name, hint.c_str());
        }
        animator.scripted = clip;
        animator.current = clip;
        animator.time = 0.0f;
        return 0;
    }
    static void getPlayerId(lua_State* L, ScriptSystem& s, scene::Entity e) {
        lua_pushnumber(L, need<scene::Player>(L, s, e, "Player").owner);
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
    {{"Lifecycle", "PlayerJoined", K::Event, A::Everywhere, "function Behaviour:PlayerJoined(player: Entity)",
      "A player joined; `player` is their character. Every behaviour receives it, after the character's own "
      "Start. Playing alone, it runs once for the player at the start."}},
    {{"Lifecycle", "PlayerLeft", K::Event, A::Everywhere, "function Behaviour:PlayerLeft(player: Entity)",
      "A player left the game. Their character is still readable here and is removed right after."}},

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
    {{"World", "tileAt", K::Function, A::Everywhere, "World.tileAt(position: vector): number?",
      "The tile number at a world position (from the tilemap drawn on top there), or nil where there is no "
      "tile.", false, true},
     &Bindings::worldTileAt},
    {{"World", "tileHasTag", K::Function, A::Everywhere, "World.tileHasTag(position: vector, tag: string): boolean",
      "True if a tile at the world position has the tag in its tileset (e.g. \"water\"; see docs/tilesets.md).",
      false, true},
     &Bindings::worldTileHasTag},
    {{"World", "spawn", K::Function, A::AuthorityOnly,
      "World.spawn(prefab: string, position: vector?, parent: Entity?): Entity",
      "Creates a copy of a prefab (by its name in the project) and returns its root. It is placed at `position` "
      "(world; default: the parent's position, or [0, 0]), under `parent` if given. The entities exist at once; "
      "their scripts start at the next tick. At most 1000 entities can be spawned per tick."},
     &Bindings::worldSpawn},

    // Input: named actions from the project's input settings.
    {{"Input", "isDown", K::Function, A::Everywhere, "Input.isDown(action: string, player: Entity?): boolean",
      "True while the action is held this tick: by `player` (a character) if given, otherwise by any player. "
      "Unknown action names are errors.", false, true},
     &Bindings::inputIsDown},
    {{"Input", "wasPressed", K::Function, A::Everywhere, "Input.wasPressed(action: string, player: Entity?): boolean",
      "True on the tick the action went down (for `player`, or for any player).", false, true},
     &Bindings::inputWasPressed},

    // Players: the characters of the people playing.
    {{"Players", "getAll", K::Function, A::Everywhere, "Players.getAll(): {Entity}",
      "Every player character (entities with a Player component), in the order they joined.", false, true},
     &Bindings::playersGetAll},
    {{"Players", "count", K::Function, A::Everywhere, "Players.count(): number",
      "How many player characters there are.", false, true},
     &Bindings::playersCount},

    // Data: saved player data.
    {{"Data", "get", K::Function, A::AuthorityOnly, "Data.get(player: Entity, key: string): any?",
      "The value saved for `player` (a character) under `key`, or nil. On the platform saved data is kept per "
      "game and account; played locally it lasts until the program closes.", false, true},
     &Bindings::dataGet},
    {{"Data", "set", K::Function, A::AuthorityOnly, "Data.set(player: Entity, key: string, value: any?)",
      "Saves a number, boolean, string or vector for `player` under `key`; nil removes it. Keys are 1-50 "
      "letters, digits, '_', '-' or '.'; a player has at most 200 values (64 KiB) per game."},
     &Bindings::dataSet},
    {{"Data", "increment", K::Function, A::AuthorityOnly,
      "Data.increment(player: Entity, key: string, amount: number?): number",
      "Adds `amount` (default 1) to a saved number (missing counts as 0) and returns the new value."},
     &Bindings::dataIncrement},

    // Leaderboard: best scores shared between players.
    {{"Leaderboard", "submit", K::Function, A::AuthorityOnly,
      "Leaderboard.submit(player: Entity, board: string, score: number): boolean",
      "Records a score for `player` (a character) on a leaderboard declared in project.json; each player keeps "
      "their best. Returns true when the score is their new best. On the platform the scores of every player "
      "of the game are kept; played locally they last until the program closes."},
     &Bindings::leaderboardSubmit},
    {{"Leaderboard", "best", K::Function, A::AuthorityOnly, "Leaderboard.best(player: Entity, board: string): number?",
      "The player's best score on the board, or nil.", false, true},
     &Bindings::leaderboardBest},
    {{"Leaderboard", "rank", K::Function, A::AuthorityOnly, "Leaderboard.rank(player: Entity, board: string): number?",
      "The player's place on the board (1 is best; equal scores share a place), or nil without a score.", false, true},
     &Bindings::leaderboardRank},
    {{"Leaderboard", "top", K::Function, A::AuthorityOnly,
      "Leaderboard.top(board: string, count: number?): { {rank: number, name: string, score: number} }",
      "The best `count` (default 10, at most 100) scores, best first, as tables with rank, name and score. "
      "On the platform the list is refreshed from it every few seconds.", false, true},
     &Bindings::leaderboardTop},

    // Sound: sounds and music (docs/audio.md).
    {{"Sound", "play", K::Function, A::AuthorityOnly, "Sound.play(name: string, volume: number?, pitch: number?)",
      "Plays a sound (by its name in the project) for everyone, the same everywhere. volume 1 is as it is (0 to "
      "4); pitch 2 is an octave up and twice as fast (0.25 to 4). At most 32 sounds start per tick."},
     &Bindings::soundPlay},
    {{"Sound", "playAt", K::Function, A::AuthorityOnly,
      "Sound.playAt(name: string, position: vector, volume: number?, pitch: number?)",
      "Plays a sound from a point in the world: full volume while it is on screen, fading out beyond, and "
      "panned to its side."},
     &Bindings::soundPlayAt},
    {{"Sound", "playFor", K::Function, A::AuthorityOnly,
      "Sound.playFor(player: Entity, name: string, volume: number?, pitch: number?)",
      "Plays a sound only `player` (a character) hears, the same everywhere: a reward, a warning."},
     &Bindings::soundPlayFor},
    {{"Sound", "playMusic", K::Function, A::AuthorityOnly,
      "Sound.playMusic(name: string, volume: number?, fadeSeconds: number?)",
      "Plays music for everyone, looping, fading over from the music before (default 1 second). The same music "
      "again only changes its volume."},
     &Bindings::soundPlayMusic},
    {{"Sound", "playMusicFor", K::Function, A::AuthorityOnly,
      "Sound.playMusicFor(player: Entity, name: string?, volume: number?, fadeSeconds: number?)",
      "Plays music only `player` hears instead of everyone's (an area of the level, a boss). nil puts them back "
      "on everyone's music."},
     &Bindings::soundPlayMusicFor},
    {{"Sound", "stopMusic", K::Function, A::AuthorityOnly, "Sound.stopMusic(fadeSeconds: number?)",
      "Fades everyone's music out (default 1 second)."},
     &Bindings::soundStopMusic},
    {{"Sound", "music", K::Function, A::AuthorityOnly, "Sound.music(): string",
      "The name of everyone's music now (empty: none).", false, true},
     &Bindings::soundMusic},

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
    {{"Entity", "visible", K::Property, A::Everywhere, "entity.visible: boolean",
      "Whether the entity is drawn (its Sprite and its Text).",
      true, true}, nullptr, &Bindings::getVisible, &Bindings::setVisible},
    {{"Entity", "flipX", K::Property, A::Everywhere, "entity.flipX: boolean", "Mirrors the Sprite horizontally.",
      true, true}, nullptr, &Bindings::getFlipX, &Bindings::setFlipX},
    {{"Entity", "text", K::Property, A::Everywhere, "entity.text: string",
      "What the entity's Text says. Assign to change it (at most 2000 bytes), e.g. "
      "`self.entity.text = \"Coins: \" .. coins`.", true, true},
     nullptr, &Bindings::getText, &Bindings::setText},
    {{"Entity", "textColor", K::Property, A::Everywhere, "entity.textColor: string",
      "The colour of the Text as \"#rrggbbaa\"; assign \"#rrggbb\" or \"#rrggbbaa\".", true, true},
     nullptr, &Bindings::getTextColor, &Bindings::setTextColor},
    {{"Entity", "soundPlaying", K::Property, A::Everywhere, "entity.soundPlaying: boolean",
      "Whether the entity's AudioSource plays. Assign to start or stop it.", true, true},
     nullptr, &Bindings::getSoundPlaying, &Bindings::setSoundPlaying},
    {{"Entity", "animation", K::Property, A::Everywhere, "entity.animation: string",
      "Asset name of the animation clip the Animator is playing (empty if none).", false, true},
     nullptr, &Bindings::getAnimation},
    {{"Entity", "spawnPoint", K::Property, A::Everywhere, "entity.spawnPoint: vector",
      "Where a Player respawns (its starting position unless changed). Set it in a checkpoint to make the player "
      "come back there.", true, true},
     nullptr, &Bindings::getSpawnPoint, &Bindings::setSpawnPoint},
    {{"Entity", "playerName", K::Property, A::Everywhere, "entity.playerName: string",
      "The name of the player controlling this character (playing alone: the character's name).", false, true},
     nullptr, &Bindings::getPlayerName, nullptr},
    {{"Entity", "playerId", K::Property, A::Everywhere, "entity.playerId: number",
      "The id of the player controlling this character in multiplayer (1, 2, ...; 0 when playing alone).",
      false, true},
     nullptr, &Bindings::getPlayerId, nullptr},
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
    {{"Entity", "playAnimation", K::Method, A::AuthorityOnly, "entity:playAnimation(name: string?)",
      "Plays the animation clip with this asset name from its start (needs an Animator). A looping clip plays "
      "until another is chosen; a clip that does not loop plays once, then the Animator chooses again (idle, "
      "run, ...). nil goes back to the automatic choice at once."},
     &Bindings::entityPlayAnimation},
    {{"Entity", "playSound", K::Method, A::AuthorityOnly, "entity:playSound(name: string, volume: number?, pitch: number?)",
      "Plays a sound from the entity, following it while it plays (louder on screen, panned to its side)."},
     &Bindings::entityPlaySound},
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
    for (const char* group : {"Game", "World", "Input", "Players", "Data", "Leaderboard", "Sound", "Events"}) {
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

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ')) s.remove_suffix(1);
    return s;
}

ApiParam parseTyped(std::string_view text, bool named) {
    ApiParam p;
    text = trim(text);
    if (named) {
        const std::size_t colon = text.find(':');
        p.name = std::string(trim(text.substr(0, colon)));
        text = colon == std::string_view::npos ? std::string_view() : trim(text.substr(colon + 1));
    }
    if (!text.empty() && text.back() == '?') {
        p.optional = true;
        text.remove_suffix(1);
    }
    p.type = std::string(trim(text));
    return p;
}

} // namespace

ApiSignature parseApiSignature(const ApiMember& member) {
    ApiSignature sig;
    std::string_view text = member.signature;
    if (member.kind == ApiKind::Property) {
        const std::size_t colon = text.rfind(": ");
        if (colon != std::string_view::npos) sig.result = parseTyped(text.substr(colon + 2), false);
        return sig;
    }
    const std::size_t open = text.find('(');
    if (open == std::string_view::npos) return sig;
    // The matching ')' (parameters may contain function types with parentheses).
    int depth = 0;
    std::size_t close = std::string_view::npos;
    for (std::size_t i = open; i < text.size(); ++i) {
        if (text[i] == '(' || text[i] == '{') ++depth;
        if (text[i] == ')' || text[i] == '}') {
            --depth;
            if (depth == 0) {
                close = i;
                break;
            }
        }
    }
    if (close == std::string_view::npos) return sig;
    const std::string_view params = text.substr(open + 1, close - open - 1);
    depth = 0;
    std::size_t start = 0;
    const auto addParam = [&](std::string_view p) {
        p = trim(p);
        if (p.empty()) return;
        if (p.starts_with("...")) {
            sig.variadic = true;
            return;
        }
        ApiParam param = parseTyped(p, true);
        if (param.type.find("->") != std::string::npos) sig.takesFunction = true;
        sig.params.push_back(std::move(param));
    };
    for (std::size_t i = 0; i < params.size(); ++i) {
        const char c = params[i];
        if (c == '(' || c == '{') ++depth;
        if (c == ')' || c == '}') --depth;
        if (c == ',' && depth == 0) {
            addParam(params.substr(start, i - start));
            start = i + 1;
        }
    }
    addParam(params.substr(start));
    const std::string_view rest = trim(text.substr(close + 1));
    if (rest.starts_with(":")) sig.result = parseTyped(rest.substr(1), false);
    return sig;
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
    const char* groups[] = {"Lifecycle", "Globals",     "Game",  "World",  "Input", "Players",
                            "Data",      "Leaderboard", "Sound", "Events", "Entity"};
    const char* intro[] = {
        "Functions a behaviour table may define. The engine calls them; see the lifecycle rules in scripting.md.",
        "Global functions.",
        "Game state. Changes are authoritative and, in multiplayer, will be replicated to clients.",
        "Finding entities in the running scene.",
        "Input actions defined in the project's `input.actions`.",
        "The people playing. Each has a character: a copy of the scene's Player entity in multiplayer, the "
        "scene's Player entity when playing alone.",
        "Saved player data: values kept for each player between visits (docs/platform.md#saved-player-data). "
        "Loaded before PlayerJoined, saved while they play and when they leave.",
        "Leaderboards declared in project.json (`leaderboards`): each player's best score, shared between "
        "everyone who plays the game on the platform (docs/leaderboards.md).",
        "Sound effects and music (docs/audio.md). Sounds are named by their asset name, as Studio shows it (e.g. \"Coin\"). "
        "Volume 1 (the default) plays a sound as recorded, up to 4; pitch is 0.25..4 (2 is an octave up and twice as fast). "
        "At most 32 sounds start per tick; more are an error. In multiplayer every player hears them "
        "unless they are played for one player.",
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
