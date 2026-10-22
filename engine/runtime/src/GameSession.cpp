#include "px/runtime/GameSession.hpp"

#include "px/core/Log.hpp"

#include <algorithm>
#include <cmath>

namespace px::runtime {

using namespace px::scene;

GameSession::GameSession(const project::ProjectManifest& project, Scene scene)
    : m_scene(std::move(scene)),
      m_actions(project.input),
      m_viewSize(static_cast<float>(project.settings.display.virtualResolution.x),
                 static_cast<float>(project.settings.display.virtualResolution.y)),
      m_dt(1.0f / static_cast<float>(std::max(1, project.settings.tickRate))) {
    m_moveLeft = m_actions.find(actions::kMoveLeft);
    m_moveRight = m_actions.find(actions::kMoveRight);
    m_jump = m_actions.find(actions::kJump);

    auto& reg = m_scene.registry();
    if (reg.count<PlatformerController>() > 0 &&
        (m_moveLeft == kInvalidAction || m_moveRight == kInvalidAction || m_jump == kInvalidAction)) {
        log::warn("Scene uses PlatformerController but the project does not define all of the "
                  "'move_left', 'move_right' and 'jump' input actions");
    }

    reg.each<Player, Transform>([](Entity, Player& player, Transform& t) { player.spawnPoint = t.position; });
    m_state.collectiblesTotal = static_cast<int>(reg.count<Collectible>());

    buildStaticCollision();
    updateCameras();
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
    auto& reg = m_scene.registry();

    reg.each<Tilemap, Transform>([&](Entity, Tilemap& map, Transform& t) {
        const Rect bounds = map.worldBounds(t.position);
        m_worldBounds = haveBounds ? unite(m_worldBounds, bounds) : bounds;
        haveBounds = true;
        if (!map.solid) return;
        physics::SolidGrid grid;
        grid.origin = t.position;
        grid.cellSize = map.tileSize;
        grid.width = map.width;
        grid.height = map.height;
        grid.solid.resize(map.tiles.size());
        for (std::size_t i = 0; i < map.tiles.size(); ++i) grid.solid[i] = map.tiles[i] >= 0 ? 1 : 0;
        m_collision.addGrid(std::move(grid));
    });

    // Solid colliders without a KinematicBody are static level geometry.
    reg.each<BoxCollider, Transform>([&](Entity e, BoxCollider& c, Transform& t) {
        if (c.isTrigger || reg.has<KinematicBody>(e)) return;
        m_collision.addBox(c.worldRect(t.position));
    });
}

void GameSession::tick(const InputFrame& input) {
    m_events.clear();
    updatePlatformers(input);
    moveKinematicBodies();
    updateCollectibles();
    updateKillPlane();
    updateAnimations();
    updateCameras();
    ++m_tick;
}

void GameSession::updatePlatformers(const InputFrame& input) {
    const float dt = m_dt;
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

void GameSession::moveKinematicBodies() {
    const float dt = m_dt;
    auto& reg = m_scene.registry();
    reg.each<KinematicBody, BoxCollider, Transform>([&](Entity, KinematicBody& body, BoxCollider& c, Transform& t) {
        const Rect box = c.worldRect(t.position);
        const physics::MoveResult r = m_collision.moveBox(box, body.velocity * dt);
        t.position += r.moved;
        body.onGround = r.hitBottom;
        body.hitCeiling = r.hitTop;
        body.hitWall = r.hitLeft || r.hitRight;
        if (r.hitBottom && body.velocity.y > 0.0f) body.velocity.y = 0.0f;
        if (r.hitTop && body.velocity.y < 0.0f) body.velocity.y = 0.0f;
        if ((r.hitLeft && body.velocity.x < 0.0f) || (r.hitRight && body.velocity.x > 0.0f)) body.velocity.x = 0.0f;
    });
}

void GameSession::updateCollectibles() {
    auto& reg = m_scene.registry();
    std::vector<Rect> playerBoxes;
    reg.each<Player, BoxCollider, Transform>(
        [&](Entity, Player&, BoxCollider& c, Transform& t) { playerBoxes.push_back(c.worldRect(t.position)); });
    if (playerBoxes.empty()) return;

    std::vector<Entity> collected;
    reg.each<Collectible, BoxCollider, Transform>([&](Entity e, Collectible& item, BoxCollider& c, Transform& t) {
        const Rect box = c.worldRect(t.position);
        for (const Rect& p : playerBoxes) {
            if (!p.intersects(box)) continue;
            m_state.score += item.value;
            ++m_state.collectiblesCollected;
            const auto* info = reg.tryGet<EntityInfo>(e);
            m_events.push_back({GameEvent::Type::Collected, info ? info->id : Guid{}, item.kind, item.value});
            collected.push_back(e);
            break;
        }
    });
    for (const Entity e : collected) m_scene.destroyEntity(e);

    if (!m_state.allCollected && m_state.collectiblesTotal > 0 &&
        m_state.collectiblesCollected >= m_state.collectiblesTotal) {
        m_state.allCollected = true;
        m_events.push_back({GameEvent::Type::AllCollected, Guid{}, {}, m_state.score});
    }
}

void GameSession::updateKillPlane() {
    const auto& killPlane = m_scene.settings().killPlaneY;
    if (!killPlane) return;
    auto& reg = m_scene.registry();
    reg.each<Player, Transform>([&](Entity e, Player& player, Transform& t) {
        if (t.position.y <= *killPlane) return;
        t.position = player.spawnPoint;
        if (auto* body = reg.tryGet<KinematicBody>(e)) *body = KinematicBody{};
        if (auto* c = reg.tryGet<PlatformerController>(e)) {
            c->coyoteTimer = 0.0f;
            c->jumpBufferTimer = 0.0f;
            c->jumping = false;
        }
        ++m_state.respawns;
        const auto* info = reg.tryGet<EntityInfo>(e);
        m_events.push_back({GameEvent::Type::PlayerRespawned, info ? info->id : Guid{}, {}, 0});
    });
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
        if (const auto* t = reg.tryGet<Transform>(e)) target = t->position;
        if (!cam.follow.isNil()) {
            const Entity followed = m_scene.findById(cam.follow);
            if (const auto* ft = reg.tryGet<Transform>(followed)) target = ft->position;
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

} // namespace px::runtime
