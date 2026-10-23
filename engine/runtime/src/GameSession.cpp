#include "px/runtime/GameSession.hpp"

#include "px/core/Log.hpp"

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

GameSession::GameSession(const project::ProjectManifest& project, Scene scene)
    : m_scene(std::move(scene)),
      m_actions(project.input),
      m_viewSize(static_cast<float>(project.settings.display.virtualResolution.x),
                 static_cast<float>(project.settings.display.virtualResolution.y)),
      m_dt(1.0f / static_cast<float>(std::max(1, project.settings.tickRate))) {
    m_moveLeft = m_actions.find(actions::kMoveLeft);
    m_moveRight = m_actions.find(actions::kMoveRight);
    m_jump = m_actions.find(actions::kJump);
    m_moveUp = m_actions.find(actions::kMoveUp);
    m_moveDown = m_actions.find(actions::kMoveDown);
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
    if (reg.count<PlatformerController>() > 0 &&
        (session->m_moveLeft == kInvalidAction || session->m_moveRight == kInvalidAction ||
         session->m_jump == kInvalidAction)) {
        log::warn("Scene uses PlatformerController but the project does not define all of the "
                  "'move_left', 'move_right' and 'jump' input actions");
    }
    if (reg.count<TopDownController>() > 0 &&
        (session->m_moveLeft == kInvalidAction || session->m_moveRight == kInvalidAction ||
         session->m_moveUp == kInvalidAction || session->m_moveDown == kInvalidAction)) {
        log::warn("Scene uses TopDownController but the project does not define all of the "
                  "'move_left', 'move_right', 'move_up' and 'move_down' input actions");
    }
    Scene& sc = session->m_scene;
    reg.each<Player, Transform>([&sc](Entity e, Player& player, Transform&) { player.spawnPoint = sc.worldPosition(e); });

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
    bool haveBounds = false;
    // Tilemaps use their world position; rotation and scale do not apply to them.
    m_scene.registry().each<Tilemap, Transform>([&](Entity e, Tilemap& map, Transform&) {
        const Vec2 origin = m_scene.worldPosition(e);
        const Rect bounds = map.worldBounds(origin);
        m_worldBounds = haveBounds ? unite(m_worldBounds, bounds) : bounds;
        haveBounds = true;
        if (!map.solid) return;
        physics::SolidGrid grid;
        grid.origin = origin;
        grid.cellSize = map.tileSize;
        grid.width = map.width;
        grid.height = map.height;
        grid.solid.reserve(map.tiles.size());
        for (const int tile : map.tiles) grid.solid.push_back(tile >= 0 ? 1 : 0);
        m_collision.addGrid(std::move(grid));
    });
}

// --- tick --------------------------------------------------------------------------------

void GameSession::tick(const InputFrame& input) {
    m_events.clear();
    m_input = input;
    m_scripts->beginTick();

    m_scripts->start();
    flushPhase();
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
    // event handlers can destroy entities: repeat until both settle.
    // Bounded by the per-tick event delivery limit.
    for (int round = 0; round < 64; ++round) {
        flushDestroyed();
        if (!m_scripts->deliverEvents() && m_pendingDestroy.empty()) return;
    }
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
        }
        m_scene.destroyEntity(root);
    }
    m_pendingDestroy.clear();
}

void GameSession::updateMovingSolids() {
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
        m_collision.addBox(c->worldRect(position), ownerOf(e));
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
    const float dt = m_dt;
    const InputFrame& input = m_input;
    auto& reg = m_scene.registry();
    reg.each<PlatformerController, KinematicBody>([&](Entity e, PlatformerController& c, KinematicBody& body) {
        const float dir = (input.isHeld(m_moveRight) ? 1.0f : 0.0f) - (input.isHeld(m_moveLeft) ? 1.0f : 0.0f);

        // Horizontal movement.
        const float target = dir * c.maxSpeed;
        float accel = c.airAcceleration;
        if (body.onGround) accel = dir != 0.0f ? c.acceleration : c.deceleration;
        body.velocity.x = approach(body.velocity.x, target, accel * dt);

        // Coyote time and jump buffering make jumps forgiving.
        if (body.onGround) {
            c.coyoteTimer = c.coyoteTime;
            c.jumping = false;
        } else {
            c.coyoteTimer = std::max(0.0f, c.coyoteTimer - dt);
        }
        if (input.wasPressed(m_jump)) {
            c.jumpBufferTimer = c.jumpBufferTime;
        } else {
            c.jumpBufferTimer = std::max(0.0f, c.jumpBufferTimer - dt);
        }
        if (c.jumpBufferTimer > 0.0f && (body.onGround || c.coyoteTimer > 0.0f)) {
            body.velocity.y = -c.jumpSpeed;
            c.jumping = true;
            c.jumpBufferTimer = 0.0f;
            c.coyoteTimer = 0.0f;
            body.onGround = false;
        }
        // Releasing jump early cuts the jump short.
        if (c.jumping && !input.isHeld(m_jump) && body.velocity.y < 0.0f) {
            body.velocity.y *= c.jumpCutMultiplier;
            c.jumping = false;
        }

        body.velocity.y = std::min(body.velocity.y + c.gravity * dt, c.maxFallSpeed);

        if (dir != 0.0f) {
            if (auto* sprite = reg.tryGet<Sprite>(e)) sprite->flipX = dir < 0.0f;
        }
    });
}

void GameSession::updateTopDown() {
    const float dt = m_dt;
    const InputFrame& input = m_input;
    auto& reg = m_scene.registry();
    reg.each<TopDownController, KinematicBody>([&](Entity e, TopDownController& c, KinematicBody& body) {
        Vec2 dir{(input.isHeld(m_moveRight) ? 1.0f : 0.0f) - (input.isHeld(m_moveLeft) ? 1.0f : 0.0f),
                 (input.isHeld(m_moveDown) ? 1.0f : 0.0f) - (input.isHeld(m_moveUp) ? 1.0f : 0.0f)};
        // Diagonals are as fast as straight lines (a constant, not a square
        // root, keeps the result identical everywhere).
        if (dir.x != 0.0f && dir.y != 0.0f) dir = dir * 0.70710678f;
        const bool moving = dir.x != 0.0f || dir.y != 0.0f;
        const Vec2 target = dir * c.maxSpeed;
        const float maxDelta = (moving ? c.acceleration : c.deceleration) * dt;
        // Each axis approaches its target, so starting and stopping feel the
        // same in every direction.
        body.velocity.x = approach(body.velocity.x, target.x, maxDelta);
        body.velocity.y = approach(body.velocity.y, target.y, maxDelta);
        if (moving) {
            c.facing = dir;
            if (dir.x != 0.0f) {
                if (auto* sprite = reg.tryGet<Sprite>(e)) sprite->flipX = dir.x < 0.0f;
            }
        }
    });
}

void GameSession::moveKinematicBodies() {
    const float dt = m_dt;
    m_scene.registry().each<KinematicBody, BoxCollider, Transform>(
        [&](Entity e, KinematicBody& body, BoxCollider& c, Transform& t) {
            const Rect box = c.worldRect(m_scene.worldPosition(e));
            const physics::MoveResult r = m_collision.moveBox(box, body.velocity * dt);
            moveWorld(m_scene, e, t, r.moved);
            body.onGround = r.hitBottom;
            body.hitCeiling = r.hitTop;
            body.hitWall = r.hitLeft || r.hitRight;
            body.ground = kNullEntity;
            if (r.hitBottom && r.groundOwner != physics::kNoOwner) {
                for (const SolidState& s : m_solids) {
                    if (ownerOf(s.entity) == r.groundOwner) {
                        body.ground = s.entity;
                        break;
                    }
                }
            }
            if (r.hitBottom && body.velocity.y > 0.0f) body.velocity.y = 0.0f;
            if (r.hitTop && body.velocity.y < 0.0f) body.velocity.y = 0.0f;
            if ((r.hitLeft && body.velocity.x < 0.0f) || (r.hitRight && body.velocity.x > 0.0f)) {
                body.velocity.x = 0.0f;
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
    if (auto* body = reg.tryGet<KinematicBody>(e)) *body = KinematicBody{};
    if (auto* c = reg.tryGet<PlatformerController>(e)) {
        c->coyoteTimer = 0.0f;
        c->jumpBufferTimer = 0.0f;
        c->jumping = false;
    }
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
}

void GameSession::updateCameras() {
    auto& reg = m_scene.registry();
    reg.each<Camera>([&](Entity e, Camera& cam) {
        Vec2 target = cam.position;
        if (reg.tryGet<Transform>(e) != nullptr) target = m_scene.worldPosition(e);
        if (!cam.follow.isNil()) {
            const Entity followed = m_scene.findById(cam.follow);
            if (reg.tryGet<Transform>(followed) != nullptr) target = m_scene.worldPosition(followed);
        }
        if (cam.clampToBounds && !m_worldBounds.empty()) {
            const Vec2 half = m_viewSize * (0.5f / cam.zoom);
            const auto clampAxis = [](float v, float lo, float hi, float halfExtent) {
                if (hi - lo <= 2.0f * halfExtent) return (lo + hi) * 0.5f; // world smaller than view: centre
                return std::clamp(v, lo + halfExtent, hi - halfExtent);
            };
            target.x = clampAxis(target.x, m_worldBounds.left(), m_worldBounds.right(), half.x);
            target.y = clampAxis(target.y, m_worldBounds.top(), m_worldBounds.bottom(), half.y);
        }
        cam.position = target;
    });
}

// --- IScriptHost -----------------------------------------------------------------------------

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
