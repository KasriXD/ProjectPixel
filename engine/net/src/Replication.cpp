#include "px/net/Replication.hpp"

#include "px/core/Version.hpp"
#include "px/net/Protocol.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace px::net {

using namespace px::scene;

namespace {

// Field values on the wire. Strings and lists are capped here as well as by
// the descriptors, so a malicious message cannot ask for huge allocations.
constexpr std::size_t kMaxNetString = 4096;
constexpr std::size_t kMaxNetList = 4096;
constexpr std::size_t kMaxEntities = 1'000'000;

void writeField(ByteWriter& w, const FieldInfo& f, const void* component) {
    switch (f.kind) {
        case FieldKind::Float: w.f32(f.ref<float>(component)); break;
        case FieldKind::Int: w.svarint(f.ref<int>(component)); break;
        case FieldKind::Bool: w.boolean(f.ref<bool>(component)); break;
        case FieldKind::String: w.string(f.ref<std::string>(component)); break;
        case FieldKind::Vec2: {
            const Vec2 v = f.ref<Vec2>(component);
            w.f32(v.x);
            w.f32(v.y);
            break;
        }
        case FieldKind::Rect: {
            const Rect r = f.ref<Rect>(component);
            w.f32(r.x);
            w.f32(r.y);
            w.f32(r.w);
            w.f32(r.h);
            break;
        }
        case FieldKind::Color: {
            const Color c = f.ref<Color>(component);
            w.u8(c.r);
            w.u8(c.g);
            w.u8(c.b);
            w.u8(c.a);
            break;
        }
        case FieldKind::Asset: w.guid(f.ref<assets::AssetId>(component).guid); break;
        case FieldKind::EntityRef: w.guid(f.ref<Guid>(component)); break;
        case FieldKind::StringList: {
            const auto& list = f.ref<std::vector<std::string>>(component);
            w.varint(list.size());
            for (const auto& s : list) w.string(s);
            break;
        }
        case FieldKind::RectList: {
            const auto& list = f.ref<std::vector<Rect>>(component);
            w.varint(list.size());
            for (const Rect& r : list) {
                w.f32(r.x);
                w.f32(r.y);
                w.f32(r.w);
                w.f32(r.h);
            }
            break;
        }
        case FieldKind::TileGrid: {
            const auto& map = *static_cast<const Tilemap*>(component);
            w.varint(static_cast<std::uint64_t>(map.width));
            w.varint(static_cast<std::uint64_t>(map.height));
            for (const int t : map.tiles) w.svarint(t);
            break;
        }
        case FieldKind::ScriptList: break; // never replicated
    }
}

// Reads one field, checking it; stores it into `component` unless null.
void readField(ByteReader& r, const FieldInfo& f, void* component) {
    const std::size_t maxString = std::min(f.maxLength, kMaxNetString);
    const std::size_t maxList = std::min(f.maxCount, kMaxNetList);
    switch (f.kind) {
        case FieldKind::Float: {
            const float v = r.f32();
            if (component != nullptr) f.ref<float>(component) = v;
            break;
        }
        case FieldKind::Int: {
            const std::int64_t v = r.svarint();
            if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) r.fail();
            if (component != nullptr) f.ref<int>(component) = static_cast<int>(v);
            break;
        }
        case FieldKind::Bool: {
            const bool v = r.boolean();
            if (component != nullptr) f.ref<bool>(component) = v;
            break;
        }
        case FieldKind::String: {
            std::string v = r.string(maxString);
            if (component != nullptr) f.ref<std::string>(component) = std::move(v);
            break;
        }
        case FieldKind::Vec2: {
            const float x = r.f32();
            const float y = r.f32();
            if (component != nullptr) f.ref<Vec2>(component) = {x, y};
            break;
        }
        case FieldKind::Rect: {
            Rect v;
            v.x = r.f32();
            v.y = r.f32();
            v.w = r.f32();
            v.h = r.f32();
            if (component != nullptr) f.ref<Rect>(component) = v;
            break;
        }
        case FieldKind::Color: {
            Color c;
            c.r = r.u8();
            c.g = r.u8();
            c.b = r.u8();
            c.a = r.u8();
            if (component != nullptr) f.ref<Color>(component) = c;
            break;
        }
        case FieldKind::Asset: {
            const Guid g = r.guid();
            if (component != nullptr) f.ref<assets::AssetId>(component) = assets::AssetId{g};
            break;
        }
        case FieldKind::EntityRef: {
            const Guid g = r.guid();
            if (component != nullptr) f.ref<Guid>(component) = g;
            break;
        }
        case FieldKind::StringList: {
            const std::size_t n = r.count(maxList, 1);
            std::vector<std::string> list;
            list.reserve(n);
            for (std::size_t i = 0; i < n && r.ok(); ++i) list.push_back(r.string(maxString));
            if (component != nullptr) f.ref<std::vector<std::string>>(component) = std::move(list);
            break;
        }
        case FieldKind::RectList: {
            const std::size_t n = r.count(maxList, 16);
            std::vector<Rect> list(n);
            for (Rect& v : list) {
                v.x = r.f32();
                v.y = r.f32();
                v.w = r.f32();
                v.h = r.f32();
            }
            if (component != nullptr) f.ref<std::vector<Rect>>(component) = std::move(list);
            break;
        }
        case FieldKind::TileGrid: {
            const std::uint64_t width = r.varint();
            const std::uint64_t height = r.varint();
            const auto limit = static_cast<std::uint64_t>(kMaxTilemapDimension);
            if (width > limit || height > limit || width * height > r.remaining()) {
                r.fail();
                break;
            }
            std::vector<int> tiles(static_cast<std::size_t>(width * height));
            for (int& t : tiles) {
                const std::int64_t v = r.svarint();
                if (v < -1 || v > std::numeric_limits<int>::max()) r.fail();
                t = static_cast<int>(v);
            }
            if (component != nullptr && r.ok()) {
                auto& map = *static_cast<Tilemap*>(component);
                map.width = static_cast<int>(width);
                map.height = static_cast<int>(height);
                map.tiles = std::move(tiles);
            }
            break;
        }
        case FieldKind::ScriptList: r.fail(); break;
    }
}

std::vector<ReplicatedComponent> buildReplicated() {
    // name -> fields that replicate (empty: all of them)
    struct Rule {
        std::string_view component;
        std::vector<std::string_view> onlyFields;
        bool interpolate;
    };
    const Rule rules[] = {
        {"Transform", {}, true},
        {"Sprite", {}, false},
        // SpriteAnimation is not sent: its result (Sprite.source) is.
        {"Tilemap", {}, false},
        {"BoxCollider", {}, false},
        {"KinematicBody", {"velocity", "onGround"}, false},
        {"PlatformerController", {}, false},
        {"TopDownController", {}, false},
        {"Player", {}, false},
        // Camera is per client (it follows that client's own character).
        {"Tags", {}, false},
        // Scripts run only on the server.
    };
    std::vector<ReplicatedComponent> out;
    for (const Rule& rule : rules) {
        const ComponentInfo* info = findComponentType(rule.component);
        if (info == nullptr) continue;
        ReplicatedComponent rc;
        rc.info = info;
        rc.interpolate = rule.interpolate;
        for (const FieldInfo& f : info->fields) {
            if (f.kind == FieldKind::ScriptList) continue;
            if (!rule.onlyFields.empty() &&
                std::find(rule.onlyFields.begin(), rule.onlyFields.end(), f.name) == rule.onlyFields.end()) {
                continue;
            }
            rc.fields.push_back(&f);
        }
        out.push_back(std::move(rc));
    }
    return out;
}

void writeGame(ByteWriter& w, const NetGameState& g) {
    w.svarint(g.score);
    w.string(g.objective);
    w.boolean(g.levelComplete);
    w.string(g.completionMessage);
    w.svarint(g.respawns);
    w.svarint(g.scriptErrors);
    w.string(g.lastScriptError);
    w.varint(g.players.size());
    for (const NetPlayer& p : g.players) {
        w.u16(p.id);
        w.string(p.name);
    }
    w.svarint(g.restartTicks);
}

int readInt(ByteReader& r) {
    const std::int64_t v = r.svarint();
    if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
        r.fail();
        return 0;
    }
    return static_cast<int>(v);
}

NetGameState readGame(ByteReader& r) {
    NetGameState g;
    g.score = readInt(r);
    g.objective = r.string(kMaxNetString);
    g.levelComplete = r.boolean();
    g.completionMessage = r.string(kMaxNetString);
    g.respawns = readInt(r);
    g.scriptErrors = readInt(r);
    g.lastScriptError = r.string(kMaxNetString);
    const std::size_t n = r.count(runtime::kMaxPlayers, 3);
    g.players.resize(n);
    for (NetPlayer& p : g.players) {
        p.id = r.u16();
        p.name = r.string(64);
    }
    g.restartTicks = readInt(r);
    return g;
}

// Index of the first field of component `c` among the present components' fields.
std::size_t firstField(std::uint32_t mask, std::size_t c) {
    std::size_t n = 0;
    const auto comps = replicatedComponents();
    for (std::size_t i = 0; i < c; ++i) {
        if ((mask >> i) & 1u) n += comps[i].fields.size();
    }
    return n;
}

void appendField(EntityState& e, std::span<const std::uint8_t> bytes) {
    e.fieldStarts.push_back(static_cast<std::uint32_t>(e.data.size()));
    e.data.insert(e.data.end(), bytes.begin(), bytes.end());
}

bool sameEntity(const EntityState& a, const EntityState& b) {
    return a.parent == b.parent && a.components == b.components && a.name == b.name && a.data == b.data;
}

enum EntityFlags : std::uint8_t {
    kNew = 1u << 0,
    kParentChanged = 1u << 1,
    kComponentsChanged = 1u << 2,
    kNameChanged = 1u << 3,
};

} // namespace

std::span<const std::uint8_t> EntityState::field(std::size_t n) const {
    if (n >= fieldStarts.size()) return {};
    const std::size_t begin = fieldStarts[n];
    const std::size_t end = n + 1 < fieldStarts.size() ? fieldStarts[n + 1] : data.size();
    return std::span<const std::uint8_t>(data).subspan(begin, end - begin);
}

const EntityState* WorldState::find(NetId id) const {
    const auto it = m_lookup.find(id);
    return it == m_lookup.end() ? nullptr : &entities[it->second];
}

void WorldState::reindex() {
    m_lookup.clear();
    m_lookup.reserve(entities.size());
    for (std::size_t i = 0; i < entities.size(); ++i) m_lookup[entities[i].id] = i;
}

std::span<const ReplicatedComponent> replicatedComponents() {
    static const std::vector<ReplicatedComponent> components = buildReplicated();
    return components;
}

void NetIdMap::assignInitial(const Scene& scene) {
    m_ids.clear();
    m_next = 1;
    for (const Entity e : scene.entities()) (void)idFor(scene.get<EntityInfo>(e).id);
}

NetId NetIdMap::idFor(const Guid& guid) {
    const auto [it, inserted] = m_ids.try_emplace(guid, m_next);
    if (inserted) ++m_next;
    return it->second;
}

NetId NetIdMap::find(const Guid& guid) const {
    const auto it = m_ids.find(guid);
    return it == m_ids.end() ? kNoNetId : it->second;
}

WorldState captureWorld(const Scene& scene, NetIdMap& ids, std::uint32_t tick) {
    WorldState world;
    world.tick = tick;
    world.entities.reserve(scene.entityCount());
    const auto comps = replicatedComponents();
    ByteWriter w;
    for (const Entity e : scene.entities()) {
        const EntityInfo& info = scene.get<EntityInfo>(e);
        EntityState s;
        s.id = ids.idFor(info.id);
        s.guid = info.id;
        s.name = info.name;
        const Entity parent = scene.parent(e);
        s.parent = parent.isNull() ? kNoNetId : ids.idFor(scene.get<EntityInfo>(parent).id);
        for (std::size_t i = 0; i < comps.size(); ++i) {
            const void* data = comps[i].info->getConst(scene, e);
            if (data == nullptr) continue;
            s.components |= 1u << i;
            for (const FieldInfo* f : comps[i].fields) {
                w.data().clear();
                writeField(w, *f, data);
                appendField(s, w.data());
            }
        }
        world.entities.push_back(std::move(s));
    }
    world.reindex();
    return world;
}

void writeWorldDelta(ByteWriter& w, const WorldState& baseline, const WorldState& current) {
    const auto comps = replicatedComponents();

    // Removed entities.
    std::vector<NetId> removed;
    for (const EntityState& b : baseline.entities) {
        if (current.find(b.id) == nullptr) removed.push_back(b.id);
    }
    w.varint(removed.size());
    for (const NetId id : removed) w.varint(id);

    // New and changed entities, in the current order (parents first).
    std::vector<const EntityState*> changed;
    for (const EntityState& c : current.entities) {
        const EntityState* b = baseline.find(c.id);
        if (b == nullptr || !sameEntity(*b, c)) changed.push_back(&c);
    }
    w.varint(changed.size());
    for (const EntityState* c : changed) {
        const EntityState* b = baseline.find(c->id);
        w.varint(c->id);
        if (b == nullptr) {
            w.u8(kNew);
            w.guid(c->guid);
            w.string(c->name);
            w.varint(c->parent);
            w.varint(c->components);
            w.bytes(c->data);
            continue;
        }
        std::uint8_t flags = 0;
        if (b->parent != c->parent) flags |= kParentChanged;
        if (b->components != c->components) flags |= kComponentsChanged;
        if (b->name != c->name) flags |= kNameChanged;
        w.u8(flags);
        if ((flags & kParentChanged) != 0) w.varint(c->parent);
        if ((flags & kNameChanged) != 0) w.string(c->name);
        if ((flags & kComponentsChanged) != 0) w.varint(c->components);
        for (std::size_t i = 0; i < comps.size(); ++i) {
            if (((c->components >> i) & 1u) == 0) continue;
            const std::size_t cFirst = firstField(c->components, i);
            const std::size_t count = comps[i].fields.size();
            if (((b->components >> i) & 1u) == 0) {
                for (std::size_t f = 0; f < count; ++f) w.bytes(c->field(cFirst + f)); // added: everything
                continue;
            }
            const std::size_t bFirst = firstField(b->components, i);
            std::uint32_t mask = 0;
            for (std::size_t f = 0; f < count; ++f) {
                const auto now = c->field(cFirst + f);
                const auto before = b->field(bFirst + f);
                if (!std::equal(now.begin(), now.end(), before.begin(), before.end())) mask |= 1u << f;
            }
            w.varint(mask);
            for (std::size_t f = 0; f < count; ++f) {
                if ((mask >> f) & 1u) w.bytes(c->field(cFirst + f));
            }
        }
    }

    // Game state.
    if (baseline.game == current.game) {
        w.boolean(false);
    } else {
        w.boolean(true);
        writeGame(w, current.game);
    }
}

Result<WorldState> readWorldDelta(ByteReader& r, const WorldState& baseline) {
    const auto comps = replicatedComponents();
    const std::uint32_t allComponents = comps.size() >= 32 ? 0xFFFFFFFFu : (1u << comps.size()) - 1u;
    const auto bad = [](std::string_view what) {
        return Error{ErrorCode::InvalidData, std::string("Malformed snapshot: ") + std::string(what)};
    };
    const auto readId = [&r]() {
        const std::uint64_t v = r.varint();
        if (v > 0xFFFFFFFFULL) r.fail();
        return static_cast<NetId>(v);
    };
    // Reads the fields of component `c` (all of them) and appends their bytes.
    const auto readAll = [&](EntityState& e, std::size_t c) {
        for (const FieldInfo* f : comps[c].fields) {
            const std::size_t start = r.position();
            readField(r, *f, nullptr);
            if (!r.ok()) return false;
            appendField(e, r.span(start));
        }
        return true;
    };

    // Start from the baseline minus removed entities.
    const std::size_t removedCount = r.count(kMaxEntities, 1);
    std::vector<NetId> removed(removedCount);
    for (NetId& id : removed) id = readId();
    if (!r.ok()) return bad("removed entities");
    std::sort(removed.begin(), removed.end());

    WorldState out;
    out.entities.reserve(baseline.entities.size());
    std::unordered_map<NetId, std::size_t> index;
    for (const EntityState& e : baseline.entities) {
        if (std::binary_search(removed.begin(), removed.end(), e.id)) continue;
        index[e.id] = out.entities.size();
        out.entities.push_back(e);
    }
    out.game = baseline.game;

    const std::size_t changedCount = r.count(kMaxEntities, 2);
    if (!r.ok()) return bad("entity count");
    for (std::size_t n = 0; n < changedCount; ++n) {
        const NetId id = readId();
        const std::uint8_t flags = r.u8();
        if (!r.ok() || id == kNoNetId || (flags & ~0x0Fu) != 0) return bad("entity header");
        const auto found = index.find(id);
        EntityState e;
        e.id = id;
        if ((flags & kNew) != 0) {
            if (found != index.end() || flags != kNew) return bad("new entity");
            e.guid = r.guid();
            e.name = r.string(kMaxNetString);
            e.parent = readId();
            const std::uint64_t mask = r.varint();
            if (!r.ok() || e.guid.isNil() || mask > allComponents) return bad("new entity");
            e.components = static_cast<std::uint32_t>(mask);
            for (std::size_t i = 0; i < comps.size(); ++i) {
                if (((e.components >> i) & 1u) != 0 && !readAll(e, i)) return bad("entity field");
            }
            index[id] = out.entities.size();
            out.entities.push_back(std::move(e));
            continue;
        }
        if (found == index.end()) return bad("change to an unknown entity");
        const EntityState& before = out.entities[found->second];
        e.guid = before.guid;
        e.name = before.name;
        e.parent = before.parent;
        e.components = before.components;
        if ((flags & kParentChanged) != 0) e.parent = readId();
        if ((flags & kNameChanged) != 0) e.name = r.string(kMaxNetString);
        if ((flags & kComponentsChanged) != 0) {
            const std::uint64_t mask = r.varint();
            if (mask > allComponents) r.fail();
            e.components = static_cast<std::uint32_t>(mask);
        }
        if (!r.ok()) return bad("entity change");
        for (std::size_t i = 0; i < comps.size(); ++i) {
            if (((e.components >> i) & 1u) == 0) continue;
            if (((before.components >> i) & 1u) == 0) {
                if (!readAll(e, i)) return bad("entity field");
                continue;
            }
            const std::size_t count = comps[i].fields.size();
            const std::size_t bFirst = firstField(before.components, i);
            const std::uint64_t mask = r.varint();
            if (!r.ok() || (count < 64 && mask >= (std::uint64_t{1} << count))) return bad("field mask");
            for (std::size_t f = 0; f < count; ++f) {
                if (((mask >> f) & 1u) == 0) {
                    appendField(e, before.field(bFirst + f));
                    continue;
                }
                const std::size_t start = r.position();
                readField(r, *comps[i].fields[f], nullptr);
                if (!r.ok()) return bad("entity field");
                appendField(e, r.span(start));
            }
        }
        out.entities[found->second] = std::move(e);
    }

    if (r.boolean()) out.game = readGame(r);
    if (!r.ok() || !r.atEnd()) return bad("game state");

    // Parents must exist, and no entity may be its own ancestor.
    for (const EntityState& e : out.entities) {
        NetId p = e.parent;
        for (int depth = 0; p != kNoNetId; ++depth) {
            const auto it = index.find(p);
            if (it == index.end() || p == e.id || depth > kMaxHierarchyDepth) return bad("entity hierarchy");
            p = out.entities[it->second].parent;
        }
    }
    out.reindex();
    return out;
}

std::uint64_t schemaHash() {
    Fnv1a64 h;
    h.addU64(kProtocolVersion);
    for (const ReplicatedComponent& c : replicatedComponents()) {
        h.add(c.info->name);
        h.addU64(c.interpolate ? 1 : 0);
        for (const FieldInfo* f : c.fields) {
            h.add(f->name);
            h.addU64(static_cast<std::uint64_t>(f->kind));
        }
    }
    return h.value();
}

std::uint64_t contentHash(const project::ProjectManifest& manifest, const WorldState& initial) {
    Fnv1a64 h;
    h.addU64(schemaHash());
    h.add(version::string);
    h.addU64(manifest.id.high());
    h.addU64(manifest.id.low());
    h.addU64(static_cast<std::uint64_t>(manifest.settings.tickRate));
    const runtime::ActionSet actions(manifest.input);
    for (std::size_t i = 0; i < actions.size(); ++i) h.add(actions.name(static_cast<runtime::ActionId>(i)));
    for (const EntityState& e : initial.entities) {
        h.addU64(e.id);
        h.addU64(e.guid.high());
        h.addU64(e.guid.low());
        h.add(e.name);
        h.addU64(e.parent);
        h.addU64(e.components);
        h.add(e.data);
    }
    return h.value();
}

// --- SceneReplica ---------------------------------------------------------------------------

SceneReplica::SceneReplica(Scene& scene) : m_scene(scene) {
    for (const Entity e : scene.entities()) m_initial.emplace(scene.get<EntityInfo>(e).id, e);
}

Entity SceneReplica::entityOf(NetId id) const {
    const auto it = m_entities.find(id);
    return it == m_entities.end() || !m_scene.isAlive(it->second.entity) ? kNullEntity : it->second.entity;
}

bool SceneReplica::readComponent(const EntityState& entity, const ComponentInfo& component, void* target) const {
    const auto comps = replicatedComponents();
    for (std::size_t i = 0; i < comps.size(); ++i) {
        if (comps[i].info != &component) continue;
        if (((entity.components >> i) & 1u) == 0) return false;
        const std::size_t first = firstField(entity.components, i);
        for (std::size_t f = 0; f < comps[i].fields.size(); ++f) {
            ByteReader r(entity.field(first + f));
            readField(r, *comps[i].fields[f], target);
            if (!r.ok()) return false;
        }
        return true;
    }
    return false;
}

void SceneReplica::apply(const WorldState& state, const WorldState* next, float alpha) {
    const auto comps = replicatedComponents();

    // 1. Every entity of the state exists here (initial entities are found by id).
    for (const EntityState& es : state.entities) {
        auto it = m_entities.find(es.id);
        if (it != m_entities.end() && m_scene.isAlive(it->second.entity)) continue;
        Entity e = m_scene.findById(es.guid);
        if (e.isNull()) {
            auto created = m_scene.createEntityWithId(es.guid, es.name);
            if (!created) continue;
            e = created.value();
        }
        m_entities[es.id] = Applied{e, {}, 0, false};
    }
    // 2. Parents.
    for (const EntityState& es : state.entities) {
        const Entity e = entityOf(es.id);
        if (e.isNull()) continue;
        const Entity wanted = es.parent == kNoNetId ? kNullEntity : entityOf(es.parent);
        if (m_scene.parent(e) != wanted) (void)m_scene.setParent(e, wanted);
    }
    // 3. Entities the state no longer has (or never had: initial ones removed before we joined).
    if (!m_initial.empty()) {
        for (const EntityState& es : state.entities) m_initial.erase(es.guid);
        for (const auto& [guid, e] : m_initial) {
            if (m_scene.isAlive(e) && m_scene.get<EntityInfo>(e).id == guid) m_scene.destroyEntity(e);
        }
        m_initial.clear();
    }
    std::vector<NetId> gone;
    for (const auto& [id, applied] : m_entities) {
        if (state.find(id) == nullptr) gone.push_back(id);
    }
    for (const NetId id : gone) {
        const Entity e = m_entities[id].entity;
        if (m_scene.isAlive(e)) m_scene.destroyEntity(e);
        m_entities.erase(id);
    }
    // 4. Components and fields of entities that changed since the last apply.
    for (const EntityState& es : state.entities) {
        auto it = m_entities.find(es.id);
        if (it == m_entities.end() || !m_scene.isAlive(it->second.entity)) continue;
        Applied& a = it->second;
        const Entity e = a.entity;
        const EntityState* towards = next != nullptr ? next->find(es.id) : nullptr;
        const bool blend = towards != nullptr && alpha > 0.0f && towards->data != es.data;
        if (!blend && !a.blended && a.components == es.components && a.data == es.data) continue;

        if (auto& info = m_scene.get<EntityInfo>(e); info.name != es.name) info.name = es.name;
        std::size_t field = 0;
        for (std::size_t i = 0; i < comps.size(); ++i) {
            const ComponentInfo& info = *comps[i].info;
            const bool present = ((es.components >> i) & 1u) != 0;
            if (!present) {
                if (info.has(m_scene, e)) info.remove(m_scene, e);
                continue;
            }
            if (!info.has(m_scene, e)) info.add(m_scene, e);
            void* target = info.get(m_scene, e);
            for (const FieldInfo* f : comps[i].fields) {
                ByteReader r(es.field(field++));
                readField(r, *f, target);
            }
            if (!blend || !comps[i].interpolate) continue;
            // Blend towards the next state (Transform: position, rotation, scale).
            Transform from = *static_cast<Transform*>(target);
            Transform to;
            if (!readComponent(*towards, info, &to)) continue;
            const Vec2 d = to.position - from.position;
            if (d.x * d.x + d.y * d.y > kTeleportDistance * kTeleportDistance) continue;
            auto& t = *static_cast<Transform*>(target);
            t.position = from.position + d * alpha;
            t.rotation = from.rotation + (to.rotation - from.rotation) * alpha;
            t.scale = from.scale + (to.scale - from.scale) * alpha;
        }
        a.data = es.data;
        a.components = es.components;
        a.blended = blend;
    }
}

} // namespace px::net
