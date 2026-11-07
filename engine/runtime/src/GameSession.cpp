#include "px/runtime/GameSession.hpp"

#include "px/core/Log.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/scene/ComponentInfo.hpp"

#include <algorithm>
#include <cmath>
#include <format>

namespace px::runtime {

using namespace px::scene;

namespace {

bool pairLess(const Entity& a1, const Entity& b1, const Entity& a2, const Entity& b2) {
    if (a1.index != a2.index) return a1.index < a2.index;
    if (a1.generation != a2.generation) return a1.generation < a2.generation;
    if (b1.index != b2.index) return b1.index < b2.index;
    return b1.generation < b2.generation;
}

// Solid owners in the collision world are entity index + 1 (0 = tiles).
physics::SolidOwner ownerOf(Entity e) { return e.index + 1; }

// Ids of a player's character (and its children): derived from the
// template's ids and the player id, so they are the same on every run.
std::uint64_t mix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

Guid characterId(const Guid& templateId, PlayerId player) {
    return Guid(mix64(templateId.high() ^ (static_cast<std::uint64_t>(player) << 48)),
                mix64(templateId.low() + player));
}

// Root ids of spawned prefab instances: the same on every run of a session.
Guid spawnedRootId(std::uint64_t seed, std::uint64_t count) {
    return Guid(mix64(0x7370617765640000ULL ^ seed ^ (count << 1)), mix64(count * 0x9e3779b97f4a7c15ULL + seed));
}

const InputFrame kNoInput{};

const assets::AssetId& firstSet(std::initializer_list<const assets::AssetId*> clips) {
    for (const assets::AssetId* c : clips) {
        if (!c->isNil()) return *c;
    }
    return **clips.begin();
}

// The clip an Animator's movement calls for (nil if none applies).
assets::AssetId movementClip(const Registry& reg, Entity e, const Animator& a) {
    const auto* body = reg.tryGet<KinematicBody>(e);
    if (body == nullptr) return {};
    constexpr float kMoving = 1.0f; // units per second
    if (reg.tryGet<PlatformerController>(e) != nullptr) {
        if (!body->onGround) {
            return body->velocity.y < 0.0f ? firstSet({&a.jump, &a.fall, &a.idle}) : firstSet({&a.fall, &a.jump, &a.idle});
        }
        return std::abs(body->velocity.x) > kMoving ? firstSet({&a.run, &a.idle}) : a.idle;
    }
    if (reg.tryGet<TopDownController>(e) != nullptr) {
        const bool moving = std::abs(body->velocity.x) > kMoving || std::abs(body->velocity.y) > kMoving;
        return moving ? firstSet({&a.run, &a.idle}) : a.idle;
    }
    return {};
}

// Moves an entity by a world-space offset. Root entities (the common case)
// are moved exactly; children are moved through their parent's transform.
void moveWorld(Scene& scene, Entity e, Transform& t, Vec2 delta) {
    if (scene.parent(e).isNull()) {
        t.position += delta;
    } else {
        scene.setWorldPosition(e, scene.worldPosition(e) + delta);
    }
}

} // namespace

PlayerTemplates extractPlayerTemplates(Scene& scene) {
    PlayerTemplates out;
    std::vector<Entity> roots;
    for (const Entity e : scene.entities()) {
        if (scene.tryGet<Player>(e) == nullptr) continue;
        const bool nested = std::any_of(roots.begin(), roots.end(), [&](Entity r) { return scene.isAncestor(r, e); });
        if (!nested) roots.push_back(e);
    }
    for (const Entity root : roots) {
        const Vec2 spawn = scene.worldPosition(root);
        // Templates keep their ids; each player's copy derives its ids from them.
        const Entity copy = out.scene.copySubtree(scene, root, [](const Guid& id) { return id; });
        if (!copy.isNull()) out.entries.push_back({copy, spawn});
    }
    for (const Entity root : roots) scene.destroyEntity(root);
    return out;
}

GameSession::GameSession(const project::ProjectManifest& project, Scene scene)
    : m_scene(std::move(scene)),
      m_actions(project.input),
      m_characterActions(CharacterActions::from(m_actions)),
      m_viewSize(static_cast<float>(project.settings.display.virtualResolution.x),
                 static_cast<float>(project.settings.display.virtualResolution.y)),
      m_dt(1.0f / static_cast<float>(std::max(1, project.settings.tickRate))) {
    for (const auto& entry : project.assets.entries()) {
        if (entry.type == assets::AssetType::Sound) m_soundAssets.emplace_back(entry.name, entry.id);
    }
}

GameSession::~GameSession() {
    // Scripts reference the scene; release them first.
    m_scripts.reset();
}

Result<std::unique_ptr<GameSession>> GameSession::create(const project::ProjectManifest& project, Scene scene,
                                                         std::shared_ptr<const scripting::ScriptLibrary> scripts,
                                                         const SessionOptions& options) {
    std::unique_ptr<GameSession> session(new GameSession(project, std::move(scene)));
    auto& reg = session->m_scene.registry();
    const CharacterActions& a = session->m_characterActions;
    if (reg.count<PlatformerController>() > 0 &&
        (a.left == kInvalidAction || a.right == kInvalidAction || a.jump == kInvalidAction)) {
        log::warn("Scene uses PlatformerController but the project does not define all of the "
                  "'move_left', 'move_right' and 'jump' input actions");
    }
    if (reg.count<TopDownController>() > 0 && (a.left == kInvalidAction || a.right == kInvalidAction ||
                                               a.up == kInvalidAction || a.down == kInvalidAction)) {
        log::warn("Scene uses TopDownController but the project does not define all of the "
                  "'move_left', 'move_right', 'move_up' and 'move_down' input actions");
    }
    Scene& sc = session->m_scene;
    if (options.multiplayer) {
        session->m_multiplayer = true;
        session->m_templates = extractPlayerTemplates(sc);
    }
    reg.each<Player, Transform>([&sc](Entity e, Player& player, Transform&) { player.spawnPoint = sc.worldPosition(e); });
    if (!options.multiplayer) {
        // Playing alone, PlayerJoined runs once for each character at the start.
        for (const Entity e : sc.entities()) {
            if (sc.tryGet<Player>(e) != nullptr) session->m_pendingJoins.push_back(e);
        }
    }

    session->m_prefabs = options.prefabs;
    session->m_localData = options.localData ? options.localData : std::make_shared<PlayerData>();
    session->m_boards = project.leaderboards;
    session->m_leaderboards = options.leaderboards ? options.leaderboards : std::make_shared<MemoryLeaderboardStore>();
    session->m_localPlayerName = options.localPlayerName;
    session->m_animations = options.animations;
    session->m_tilesets = options.tilesets;
    session->m_spawnCount = options.seed;

    scripting::ScriptSystemOptions scriptOptions;
    scriptOptions.realm = scripting::Realm::Authority;
    scriptOptions.limits = options.scriptLimits;
    scriptOptions.seed = options.seed;
    auto system = scripting::ScriptSystem::create(session->m_scene, std::move(scripts), *session, scriptOptions);
    if (!system) return system.error();
    session->m_scripts = std::move(system.value());

    session->buildStaticCollision();
    session->updateMovingSolids();
    session->updateCameras();
    return session;
}

Entity GameSession::player() const {
    for (const Entity e : m_scene.entities()) {
        if (m_scene.tryGet<Player>(e)) return e;
    }
    return kNullEntity;
}

void GameSession::buildStaticCollision() {
    m_collision.clear();
    addTilemapCollision(m_scene, m_collision, m_tilesets.get());
    m_worldBounds = tilemapBounds(m_scene);
}

// --- players -------------------------------------------------------------------------------

const PlayerSlot* GameSession::findPlayer(PlayerId id) const {
    for (const PlayerSlot& p : m_players) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

Entity GameSession::characterOf(PlayerId id) const {
    const PlayerSlot* slot = findPlayer(id);
    return slot != nullptr && m_scene.isAlive(slot->character) ? slot->character : kNullEntity;
}

Result<Entity> GameSession::addPlayer(PlayerId id, std::string name, PlayerData data, std::string account) {
    if (!m_multiplayer) return makeError(ErrorCode::InvalidArgument, "Players can only be added in multiplayer");
    if (id == kNoPlayer || findPlayer(id) != nullptr) {
        return makeError(ErrorCode::InvalidArgument, "Player id {} is not free", id);
    }
    if (m_players.size() >= kMaxPlayers) return makeError(ErrorCode::LimitExceeded, "The game is full");
    PlayerSlot slot;
    slot.id = id;
    slot.name = std::move(name);
    slot.data = std::move(data);
    slot.account = account.empty() ? slot.name : std::move(account);
    m_leaderboards->playerJoined(slot.account);
    if (!m_templates.entries.empty()) {
        // Players take turns over the templates, so several Player entities
        // in a scene are several spawn points.
        const auto& entry = m_templates.entries[static_cast<std::size_t>(id - 1) % m_templates.entries.size()];
        const Entity c =
            m_scene.copySubtree(m_templates.scene, entry.root, [id](const Guid& g) { return characterId(g, id); });
        if (c.isNull()) return makeError(ErrorCode::InvalidData, "Could not create a character for player {}", id);
        // References inside the character (a child following another) point to the copies.
        std::vector<std::pair<Guid, Guid>> remap;
        const std::vector<Entity> original = m_templates.scene.subtree(entry.root);
        const std::vector<Entity> copies = m_scene.subtree(c);
        for (std::size_t i = 0; i < original.size() && i < copies.size(); ++i) {
            remap.emplace_back(m_templates.scene.get<EntityInfo>(original[i]).id, m_scene.get<EntityInfo>(copies[i]).id);
        }
        remapEntityReferences(m_scene, copies, remap);
        const Vec2 spawn = freeSpawnPoint(c, entry.spawn);
        m_scene.setWorldPosition(c, spawn);
        Player& player = m_scene.get<Player>(c);
        player.owner = id;
        player.name = slot.name;
        player.spawnPoint = spawn;
        m_scripts->entityCreated(c);
        m_pendingJoins.push_back(c);
        slot.character = c;
    }
    m_players.push_back(std::move(slot));
    return m_players.back().character;
}

Vec2 GameSession::freeSpawnPoint(Entity character, Vec2 wanted) const {
    // Players sharing a spawn point stand side by side instead of on top of
    // each other: the nearest spot, a tile at a time, that is free of walls
    // and of other characters.
    const auto* box = m_scene.tryGet<BoxCollider>(character);
    const float step = box != nullptr ? std::max(20.0f, box->size.x + 6.0f) : 20.0f;
    for (int i = 0; i < 9; ++i) {
        const float offset = static_cast<float>((i + 1) / 2) * step * (i % 2 == 1 ? 1.0f : -1.0f);
        const Vec2 p{wanted.x + offset, wanted.y};
        if (box != nullptr && m_collision.overlapsSolid(box->worldRect(p))) continue;
        bool crowded = false;
        for (const PlayerSlot& other : m_players) {
            if (!m_scene.isAlive(other.character)) continue;
            const Vec2 q = m_scene.worldPosition(other.character);
            if (std::abs(q.x - p.x) < step * 0.5f && std::abs(q.y - p.y) < step * 0.5f) crowded = true;
        }
        if (!crowded) return p;
    }
    return wanted;
}

void GameSession::removePlayer(PlayerId id) {
    const auto it = std::find_if(m_players.begin(), m_players.end(), [id](const PlayerSlot& p) { return p.id == id; });
    if (it == m_players.end()) return;
    if (m_scene.isAlive(it->character)) m_pendingLeaves.push_back(it->character);
    m_leaderboards->playerLeft(it->account);
    m_players.erase(it);
}

void GameSession::setPlayerInput(PlayerId id, const InputFrame& input) {
    for (PlayerSlot& p : m_players) {
        if (p.id == id) p.input = input;
    }
}

void GameSession::deliverPlayerChanges() {
    const std::vector<Entity> leaves = std::move(m_pendingLeaves);
    const std::vector<Entity> joins = std::move(m_pendingJoins);
    m_pendingLeaves.clear();
    m_pendingJoins.clear();
    for (const Entity c : leaves) {
        if (!m_scene.isAlive(c)) continue;
        m_scripts->playerLeft(c);
        requestDestroy(c);
    }
    for (const Entity c : joins) {
        if (!m_scene.isAlive(c) || std::find(leaves.begin(), leaves.end(), c) != leaves.end()) continue;
        m_scripts->playerJoined(c);
    }
}

const InputFrame& GameSession::inputFor(Entity entity) const {
    if (!m_multiplayer) return m_input;
    const auto* player = m_scene.tryGet<Player>(entity);
    if (player == nullptr) return m_input; // other controllers follow any player's input
    const PlayerSlot* slot = findPlayer(static_cast<PlayerId>(player->owner));
    return slot != nullptr ? slot->input : kNoInput;
}

// --- tick --------------------------------------------------------------------------------

void GameSession::tick(const InputFrame& input) {
    m_events.clear();
    m_sounds.clear();
    if (m_multiplayer) {
        // Without a player argument, input means "any player".
        m_input = {};
        for (const PlayerSlot& p : m_players) {
            m_input.held |= p.input.held;
            m_input.pressed |= p.input.pressed;
        }
    } else {
        m_input = input;
    }
    m_spawnedThisTick = 0;
    m_scripts->beginTick();

    m_scripts->start();
    flushPhase();
    if (!m_pendingJoins.empty() || !m_pendingLeaves.empty()) {
        deliverPlayerChanges();
        flushPhase();
    }
    m_scripts->tick(m_dt);
    flushPhase();

    updateMovingSolids();
    updatePlatformers();
    updateTopDown();
    moveKinematicBodies();
    updateTriggers();
    updateKillPlane();
    updateAnimations();
    updateCameras();
    ++m_tick;
}

void GameSession::flushPhase() {
    // Destroying entities can run Destroy handlers that emit events, and
    // event handlers can destroy and spawn entities: repeat until all
    // settle. Bounded by the per-tick event delivery and spawn limits.
    for (int round = 0; round < 64; ++round) {
        flushCreated();
        flushDestroyed();
        if (!m_scripts->deliverEvents() && m_pendingDestroy.empty() && m_pendingCreated.empty()) return;
    }
}

void GameSession::flushCreated() {
    // Script instances of spawned entities are made here, outside the
    // script that spawned them; they start at the next tick.
    const std::vector<Entity> created = std::move(m_pendingCreated);
    m_pendingCreated.clear();
    for (const Entity root : created) m_scripts->entityCreated(root);
}

Result<Entity> GameSession::spawnPrefab(std::string_view name, Vec2 position, Entity parent) {
    const project::Prefab* prefab = m_prefabs != nullptr ? m_prefabs->findByName(name) : nullptr;
    if (prefab == nullptr) {
        const std::vector<std::string> names = prefabNames();
        if (names.empty()) return makeError(ErrorCode::NotFound, "no prefab named '{}' (this game has no prefabs)", name);
        return makeError(ErrorCode::NotFound, "no prefab named '{}'{}", name, scripting::suggestName(name, names));
    }
    if (!parent.isNull() && !m_scene.isAlive(parent)) {
        return makeError(ErrorCode::InvalidArgument, "the parent entity has been destroyed");
    }
    const std::size_t count = prefab->scene.entityCount();
    if (m_spawnedThisTick + count > kMaxSpawnedEntitiesPerTick) {
        return makeError(ErrorCode::LimitExceeded, "too many entities spawned in one tick (the limit is {})",
                         kMaxSpawnedEntitiesPerTick);
    }
    if (m_scene.entityCount() + count > project::kMaxEntitiesPerScene) {
        return makeError(ErrorCode::LimitExceeded, "the scene is full (the limit is {} entities)",
                         project::kMaxEntitiesPerScene);
    }
    Result<Entity> created = makeError(ErrorCode::Unknown, "no free id");
    for (int attempt = 0; attempt < 8; ++attempt) {
        created = project::instantiatePrefab(m_scene, prefab->scene, prefab->id, spawnedRootId(0, ++m_spawnCount),
                                             parent);
        if (created) break;
    }
    if (!created) return created.error();
    const Entity root = created.value();
    m_scene.setWorldPosition(root, position);
    m_spawnedThisTick += count;
    for (const Entity e : m_scene.subtree(root)) {
        if (m_scene.tryGet<Tilemap>(e) != nullptr) m_staticCollisionDirty = true;
        if (auto* player = m_scene.tryGet<Player>(e)) player->spawnPoint = m_scene.worldPosition(e);
    }
    m_pendingCreated.push_back(root);
    return root;
}

std::vector<std::string> GameSession::prefabNames() const {
    return m_prefabs != nullptr ? m_prefabs->names() : std::vector<std::string>{};
}

void GameSession::requestDestroy(Entity entity) {
    if (!m_scene.isAlive(entity)) return;
    if (std::find(m_pendingDestroy.begin(), m_pendingDestroy.end(), entity) != m_pendingDestroy.end()) return;
    m_pendingDestroy.push_back(entity);
}

void GameSession::flushDestroyed() {
    for (std::size_t i = 0; i < m_pendingDestroy.size(); ++i) {
        const Entity root = m_pendingDestroy[i];
        if (!m_scene.isAlive(root)) continue;
        // Destroying an entity destroys its descendants too. Each is told in
        // hierarchy order (parents first) while all of them are still readable.
        const std::vector<Entity> doomed = m_scene.subtree(root);
        for (const Entity e : doomed) {
            if (!m_scene.isAlive(e)) continue;
            // Partners of active trigger overlaps are told the overlap ended.
            std::vector<TriggerPair> ended;
            std::erase_if(m_triggerPairs, [&](const TriggerPair& p) {
                if (p.trigger != e && p.other != e) return false;
                ended.push_back(p);
                return true;
            });
            for (const TriggerPair& p : ended) {
                const Entity partner = p.trigger == e ? p.other : p.trigger;
                if (std::find(doomed.begin(), doomed.end(), partner) == doomed.end()) m_scripts->triggerExit(partner, e);
            }
            m_scripts->entityDestroyed(e); // runs Destroy(); the entity is still readable
            if (m_scene.tryGet<Tilemap>(e) != nullptr) m_staticCollisionDirty = true;
        }
        m_scene.destroyEntity(root);
    }
    m_pendingDestroy.clear();
}

void GameSession::updateMovingSolids() {
    if (m_staticCollisionDirty) {
        m_staticCollisionDirty = false;
        buildStaticCollision();
    }
    // Solid colliders without a KinematicBody are level geometry. They are
    // rebuilt every tick because scripts may move them (moving platforms).
    std::vector<SolidState> previous = std::move(m_solids);
    m_solids.clear();
    m_collision.clearBoxes();
    for (const Entity e : m_scene.entities()) {
        const auto* c = m_scene.tryGet<BoxCollider>(e);
        const auto* t = m_scene.tryGet<Transform>(e);
        if (c == nullptr || t == nullptr || c->isTrigger || m_scene.tryGet<KinematicBody>(e) != nullptr) continue;
        const Vec2 position = m_scene.worldPosition(e);
        m_collision.addBox(c->worldRect(position), ownerOf(e), colliderShape(*c));
        m_solids.push_back({e, position});
    }

    // Carry bodies that stood on a solid that moved since the last tick.
    for (const SolidState& now : m_solids) {
        const auto it = std::find_if(previous.begin(), previous.end(),
                                     [&](const SolidState& s) { return s.entity == now.entity; });
        if (it == previous.end()) continue;
        const Vec2 delta = now.position - it->position;
        if (delta.x == 0.0f && delta.y == 0.0f) continue;
        m_scene.registry().each<KinematicBody, BoxCollider, Transform>(
            [&](Entity e, KinematicBody& body, BoxCollider& c, Transform& t) {
                if (!body.onGround || body.ground != now.entity) return;
                const physics::MoveResult r = m_collision.moveBox(c.worldRect(m_scene.worldPosition(e)), delta);
                moveWorld(m_scene, e, t, r.moved);
            });
    }
}

void GameSession::updatePlatformers() {
    auto& reg = m_scene.registry();
    reg.each<PlatformerController, KinematicBody>([&](Entity e, PlatformerController& c, KinematicBody& body) {
        stepPlatformerControl(c, body, reg.tryGet<Sprite>(e), inputFor(e), m_characterActions, m_dt);
    });
}

void GameSession::updateTopDown() {
    auto& reg = m_scene.registry();
    reg.each<TopDownController, KinematicBody>([&](Entity e, TopDownController& c, KinematicBody& body) {
        stepTopDownControl(c, body, reg.tryGet<Sprite>(e), inputFor(e), m_characterActions, m_dt);
    });
}

void GameSession::moveKinematicBodies() {
    m_scene.registry().each<KinematicBody, BoxCollider, Transform>(
        [&](Entity e, KinematicBody& body, BoxCollider& c, Transform& t) {
            const physics::MoveResult r = moveBody(m_collision, body, c.worldRect(m_scene.worldPosition(e)), m_dt);
            moveWorld(m_scene, e, t, r.moved);
            body.ground = kNullEntity;
            if (r.hitBottom && r.groundOwner != physics::kNoOwner) {
                for (const SolidState& s : m_solids) {
                    if (ownerOf(s.entity) == r.groundOwner) {
                        body.ground = s.entity;
                        break;
                    }
                }
            }
        });
}

void GameSession::updateTriggers() {
    // Trigger colliders against moving bodies. O(triggers x bodies), which is
    // fine for the scene sizes of this milestone; a broadphase comes later.
    struct Box {
        Entity entity;
        Rect rect;
    };
    std::vector<Box> triggers;
    std::vector<Box> bodies;
    for (const Entity e : m_scene.entities()) {
        const auto* c = m_scene.tryGet<BoxCollider>(e);
        if (c == nullptr || m_scene.tryGet<Transform>(e) == nullptr) continue;
        if (c->isTrigger) {
            triggers.push_back({e, c->worldRect(m_scene.worldPosition(e))});
        } else if (m_scene.tryGet<KinematicBody>(e) != nullptr) {
            bodies.push_back({e, c->worldRect(m_scene.worldPosition(e))});
        }
    }
    std::vector<TriggerPair> current;
    for (const Box& trig : triggers) {
        for (const Box& body : bodies) {
            if (trig.entity != body.entity && trig.rect.intersects(body.rect)) current.push_back({trig.entity, body.entity});
        }
    }
    const auto less = [](const TriggerPair& a, const TriggerPair& b) {
        return pairLess(a.trigger, a.other, b.trigger, b.other);
    };
    std::sort(current.begin(), current.end(), less);

    std::vector<TriggerPair> entered;
    std::vector<TriggerPair> exited;
    std::set_difference(current.begin(), current.end(), m_triggerPairs.begin(), m_triggerPairs.end(),
                        std::back_inserter(entered), less);
    std::set_difference(m_triggerPairs.begin(), m_triggerPairs.end(), current.begin(), current.end(),
                        std::back_inserter(exited), less);
    m_triggerPairs = std::move(current);

    for (const TriggerPair& p : exited) {
        if (m_scene.isAlive(p.trigger) && m_scene.isAlive(p.other)) {
            m_scripts->triggerExit(p.trigger, p.other);
            m_scripts->triggerExit(p.other, p.trigger);
        }
    }
    for (const TriggerPair& p : entered) {
        // An earlier handler in this phase may have destroyed one of them.
        if (!m_scene.isAlive(p.trigger) || !m_scene.isAlive(p.other)) continue;
        if (std::find(m_pendingDestroy.begin(), m_pendingDestroy.end(), p.trigger) != m_pendingDestroy.end()) continue;
        if (std::find(m_pendingDestroy.begin(), m_pendingDestroy.end(), p.other) != m_pendingDestroy.end()) continue;
        m_scripts->triggerEnter(p.trigger, p.other);
        m_scripts->triggerEnter(p.other, p.trigger);
    }
    flushPhase();
}

void GameSession::updateKillPlane() {
    const auto& killPlane = m_scene.settings().killPlaneY;
    if (!killPlane) return;
    auto& reg = m_scene.registry();
    std::vector<Entity> fallen;
    reg.each<Player, Transform>([&](Entity e, Player&, Transform&) {
        if (m_scene.worldPosition(e).y > *killPlane) fallen.push_back(e);
    });
    for (const Entity e : fallen) (void)respawnPlayer(e);
}

bool GameSession::respawnPlayer(Entity e) {
    auto& reg = m_scene.registry();
    auto* player = reg.tryGet<Player>(e);
    if (player == nullptr || reg.tryGet<Transform>(e) == nullptr) return false;
    m_scene.setWorldPosition(e, player->spawnPoint);
    resetMotion(reg.tryGet<KinematicBody>(e), reg.tryGet<PlatformerController>(e));
    ++m_state.respawns;
    const auto* info = reg.tryGet<EntityInfo>(e);
    m_events.push_back({GameEvent::Type::PlayerRespawned, info ? info->id : Guid{}, {}, 0});
    return true;
}

void GameSession::updateAnimations() {
    const float dt = m_dt;
    m_scene.registry().each<SpriteAnimation, Sprite>([dt](Entity, SpriteAnimation& anim, Sprite& sprite) {
        if (anim.frames.empty()) return;
        if (anim.playing) anim.time += dt;
        const auto count = static_cast<long long>(anim.frames.size());
        auto frame = static_cast<long long>(std::floor(anim.time * anim.framesPerSecond));
        if (anim.loop) {
            frame %= count;
        } else if (frame >= count) {
            frame = count - 1;
            anim.playing = false;
        }
        sprite.source = anim.frames[static_cast<std::size_t>(frame)];
    });

    // Animators: a script's clip, else the one the movement calls for, else the default.
    if (m_animations == nullptr) return;
    auto& reg = m_scene.registry();
    reg.each<Animator, Sprite>([&](Entity e, Animator& a, Sprite& sprite) {
        assets::AssetId wanted = a.scripted;
        if (wanted.isNil()) wanted = movementClip(reg, e, a);
        if (wanted.isNil()) wanted = a.clip;
        if (wanted != a.current) {
            a.current = wanted;
            a.time = 0.0f;
        }
        const auto* entry = m_animations->find(a.current);
        if (entry == nullptr || entry->clip.frames.empty()) return;
        const project::AnimationClip& clip = entry->clip;
        if (a.playing) a.time += dt * a.speed;
        sprite.texture = clip.texture;
        sprite.source = clip.frames[clip.frameAt(a.time)];
        // A clip that does not loop, played by a script, hands back to the automatic choice when done.
        if (!a.scripted.isNil() && clip.finishedAt(a.time)) a.scripted = {};
    });
}

void GameSession::updateCameras() { runtime::updateCameras(m_scene, m_worldBounds, m_viewSize); }

// --- IScriptHost -----------------------------------------------------------------------------

assets::AssetId GameSession::findAnimation(std::string_view name) const {
    const auto* entry = m_animations != nullptr ? m_animations->findByName(name) : nullptr;
    return entry != nullptr ? entry->id : assets::AssetId{};
}

std::string GameSession::animationName(assets::AssetId clip) const {
    const auto* entry = m_animations != nullptr ? m_animations->find(clip) : nullptr;
    return entry != nullptr ? entry->name : std::string();
}

// --- sounds and music ----------------------------------------------------------------------

assets::AssetId GameSession::findSound(std::string_view name) const {
    for (const auto& [n, id] : m_soundAssets) {
        if (n == name) return id;
    }
    return {};
}

std::string GameSession::soundName(assets::AssetId sound) const {
    for (const auto& [n, id] : m_soundAssets) {
        if (id == sound) return n;
    }
    return {};
}

std::vector<std::string> GameSession::soundNames() const {
    std::vector<std::string> names;
    for (const auto& entry : m_soundAssets) names.push_back(entry.first);
    return names;
}

Status GameSession::playSound(const scripting::SoundRequest& request) {
    if (m_sounds.size() >= kMaxSoundsPerTick) {
        return makeError(ErrorCode::LimitExceeded, "too many sounds this tick (at most {})", kMaxSoundsPerTick);
    }
    SoundEvent event;
    event.sound = request.sound;
    event.volume = request.volume;
    event.pitch = request.pitch;
    event.position = request.position;
    if (!request.at.isNull()) {
        const auto* info = m_scene.tryGet<EntityInfo>(request.at);
        if (info != nullptr) event.entity = info->id;
        if (m_scene.tryGet<Transform>(request.at) != nullptr) event.position = m_scene.worldPosition(request.at);
    }
    if (!request.listener.isNull()) {
        const auto* player = m_scene.tryGet<Player>(request.listener);
        if (player == nullptr) return makeError(ErrorCode::InvalidArgument, "the listener is not a player's character");
        event.player = static_cast<PlayerId>(player->owner);
    }
    m_sounds.push_back(event);
    return {};
}

Status GameSession::playMusic(Entity player, assets::AssetId sound, float volume, float fadeSeconds) {
    MusicState music{sound, volume, fadeSeconds};
    if (player.isNull()) {
        if (sound.isNil() && m_state.music.sound.isNil()) return {};
        if (sound == m_state.music.sound) music.fadeSeconds = m_state.music.fadeSeconds; // only the volume changes
        m_state.music = music;
        return {};
    }
    const auto* character = m_scene.tryGet<Player>(player);
    if (character == nullptr) return makeError(ErrorCode::InvalidArgument, "the entity is not a player's character");
    std::optional<MusicState> own;
    if (!sound.isNil()) own = music;
    if (!m_multiplayer) {
        m_localMusic = own;
        return {};
    }
    for (PlayerSlot& slot : m_players) {
        if (slot.id == character->owner) slot.music = own;
    }
    return {};
}

const MusicState& GameSession::musicFor(PlayerId player) const {
    if (!m_multiplayer) return m_localMusic ? *m_localMusic : m_state.music;
    const PlayerSlot* slot = findPlayer(player);
    return slot != nullptr && slot->music ? *slot->music : m_state.music;
}

// --- saved player data ---------------------------------------------------------------------

std::vector<PlayerId> GameSession::takeChangedPlayerData() {
    std::vector<PlayerId> out;
    for (PlayerSlot& p : m_players) {
        if (!p.dataChanged) continue;
        p.dataChanged = false;
        out.push_back(p.id);
    }
    return out;
}

const PlayerData* GameSession::dataOf(Entity player) const {
    if (!m_scene.isAlive(player) || m_scene.tryGet<Player>(player) == nullptr) return nullptr;
    if (!m_multiplayer) return m_localData.get();
    for (const PlayerSlot& p : m_players) {
        if (p.character == player) return &p.data;
    }
    return nullptr;
}

Result<std::optional<scene::ScriptValue>> GameSession::playerDataGet(Entity player, std::string_view key) const {
    const PlayerData* data = dataOf(player);
    if (data == nullptr) return makeError(ErrorCode::InvalidArgument, "the entity is not a player's character");
    if (!isValidDataKey(key)) {
        return makeError(ErrorCode::InvalidArgument, "'{}' cannot be a data key: use 1 to {} letters, digits, '_', "
                         "'-' or '.'", key, kMaxPlayerDataKeyLength);
    }
    if (const auto* v = data->get(key)) return std::optional<scene::ScriptValue>(*v);
    return std::optional<scene::ScriptValue>();
}

Status GameSession::playerDataSet(Entity player, std::string_view key, std::optional<scene::ScriptValue> value) {
    if (dataOf(player) == nullptr) return makeError(ErrorCode::InvalidArgument, "the entity is not a player's character");
    if (!m_multiplayer) return m_localData->set(key, std::move(value));
    for (PlayerSlot& p : m_players) {
        if (p.character != player) continue;
        if (auto s = p.data.set(key, std::move(value)); !s) return s;
        p.dataChanged = true;
        return {};
    }
    return makeError(ErrorCode::InvalidArgument, "the entity is not a player's character");
}

Result<const project::LeaderboardDef*> GameSession::findBoard(std::string_view board) const {
    for (const auto& b : m_boards) {
        if (b.id == board) return &b;
    }
    return makeError(ErrorCode::InvalidArgument,
                     "there is no leaderboard '{}'; declare it in project.json under \"leaderboards\"", board);
}

Result<GameSession::Scorer> GameSession::scorer(Entity player, std::string_view board) const {
    auto def = findBoard(board);
    if (!def) return def.error();
    if (!m_scene.isAlive(player) || m_scene.tryGet<Player>(player) == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "the entity is not a player's character");
    }
    if (!m_multiplayer) return Scorer{def.value(), "local", m_localPlayerName};
    for (const PlayerSlot& p : m_players) {
        if (p.character == player) return Scorer{def.value(), p.account, p.name};
    }
    return makeError(ErrorCode::InvalidArgument, "the entity is not a player's character");
}

Result<bool> GameSession::leaderboardSubmit(Entity player, std::string_view board, double score) {
    auto who = scorer(player, board);
    if (!who) return who.error();
    if (!std::isfinite(score) || std::abs(score) > 1e15) {
        return makeError(ErrorCode::InvalidArgument, "scores must be finite numbers up to 1e15");
    }
    // A script submitting in a loop must not flood the platform.
    if (m_submitTick != m_tick) {
        m_submitTick = m_tick;
        m_submitsThisTick = 0;
    }
    if (++m_submitsThisTick > 100) return makeError(ErrorCode::LimitExceeded, "at most 100 scores per tick");
    return m_leaderboards->submit(*who->board, who->account, who->name, score);
}

Result<std::optional<scripting::LeaderboardRow>> GameSession::leaderboardEntry(Entity player,
                                                                               std::string_view board) const {
    auto who = scorer(player, board);
    if (!who) return who.error();
    return m_leaderboards->entry(*who->board, who->account);
}

Result<std::vector<scripting::LeaderboardRow>> GameSession::leaderboardTop(std::string_view board,
                                                                           std::size_t count) const {
    auto def = findBoard(board);
    if (!def) return def.error();
    return m_leaderboards->top(*def.value(), count);
}

const std::vector<std::string>* GameSession::tileTags(assets::AssetId tileset, Vec2 tileSize, int tile) const {
    const project::Tileset* t = m_tilesets != nullptr ? m_tilesets->forTexture(tileset, tileSize) : nullptr;
    return t != nullptr ? &t->info(tile).tags : nullptr;
}

std::vector<std::string> GameSession::animationNames() const {
    return m_animations != nullptr ? m_animations->names() : std::vector<std::string>{};
}

int GameSession::findAction(std::string_view name) const {
    const ActionId id = m_actions.find(name);
    return id == kInvalidAction ? -1 : static_cast<int>(id);
}

std::vector<std::string> GameSession::actionNames() const {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < m_actions.size(); ++i) names.emplace_back(m_actions.name(static_cast<ActionId>(i)));
    return names;
}

bool GameSession::actionHeld(int action) const { return m_input.isHeld(static_cast<ActionId>(action)); }
bool GameSession::actionPressed(int action) const { return m_input.wasPressed(static_cast<ActionId>(action)); }
bool GameSession::actionHeldBy(Entity character, int action) const {
    return inputFor(character).isHeld(static_cast<ActionId>(action));
}
bool GameSession::actionPressedBy(Entity character, int action) const {
    return inputFor(character).wasPressed(static_cast<ActionId>(action));
}

void GameSession::addScore(int amount) {
    if (amount == 0) return;
    m_state.score += amount;
    m_events.push_back({GameEvent::Type::ScoreChanged, Guid{}, {}, m_state.score});
}

void GameSession::setObjective(std::string text) {
    if (text == m_state.objective) return;
    m_state.objective = std::move(text);
    m_events.push_back({GameEvent::Type::ObjectiveChanged, Guid{}, m_state.objective, 0});
}

void GameSession::completeLevel(std::string message) {
    if (m_state.levelComplete) return;
    m_state.levelComplete = true;
    m_state.completionMessage = std::move(message);
    m_events.push_back({GameEvent::Type::LevelCompleted, Guid{}, m_state.completionMessage, m_state.score});
}

void GameSession::scriptLog(log::Level level, std::string_view script, std::string_view message) {
    log::write(level, std::format("[{}] {}", script, message));
}

void GameSession::scriptError(const scripting::ScriptError& error) {
    ++m_state.scriptErrors;
    m_state.lastScriptError = error.describe();
    log::error("{}", m_state.lastScriptError);
    if (!error.traceback.empty()) log::debug("Traceback:\n{}", error.traceback);
    m_events.push_back({GameEvent::Type::ScriptError, Guid{}, m_state.lastScriptError, 0});
}

void GameSession::scriptEvent(std::string_view name, std::string_view valueText) {
    m_events.push_back({GameEvent::Type::ScriptEvent, Guid{}, std::format("{}: {}", name, valueText), 0});
}

} // namespace px::runtime
