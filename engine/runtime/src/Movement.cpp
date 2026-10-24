#include "px/runtime/Movement.hpp"

#include <algorithm>

namespace px::runtime {

using namespace px::scene;

CharacterActions CharacterActions::from(const ActionSet& actions) {
    CharacterActions a;
    a.left = actions.find(actions::kMoveLeft);
    a.right = actions.find(actions::kMoveRight);
    a.up = actions.find(actions::kMoveUp);
    a.down = actions.find(actions::kMoveDown);
    a.jump = actions.find(actions::kJump);
    return a;
}

void stepPlatformerControl(PlatformerController& c, KinematicBody& body, Sprite* sprite, const InputFrame& input,
                           const CharacterActions& actions, float dt) {
    const float dir = (input.isHeld(actions.right) ? 1.0f : 0.0f) - (input.isHeld(actions.left) ? 1.0f : 0.0f);

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
    if (input.wasPressed(actions.jump)) {
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
    if (c.jumping && !input.isHeld(actions.jump) && body.velocity.y < 0.0f) {
        body.velocity.y *= c.jumpCutMultiplier;
        c.jumping = false;
    }

    body.velocity.y = std::min(body.velocity.y + c.gravity * dt, c.maxFallSpeed);

    if (dir != 0.0f && sprite != nullptr) sprite->flipX = dir < 0.0f;
}

void stepTopDownControl(TopDownController& c, KinematicBody& body, Sprite* sprite, const InputFrame& input,
                        const CharacterActions& actions, float dt) {
    Vec2 dir{(input.isHeld(actions.right) ? 1.0f : 0.0f) - (input.isHeld(actions.left) ? 1.0f : 0.0f),
             (input.isHeld(actions.down) ? 1.0f : 0.0f) - (input.isHeld(actions.up) ? 1.0f : 0.0f)};
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
        if (dir.x != 0.0f && sprite != nullptr) sprite->flipX = dir.x < 0.0f;
    }
}

physics::MoveResult moveBody(const physics::CollisionWorld& world, KinematicBody& body, const Rect& box, float dt) {
    physics::MoveResult r = world.moveBox(box, body.velocity * dt);
    body.onGround = r.hitBottom;
    body.hitCeiling = r.hitTop;
    body.hitWall = r.hitLeft || r.hitRight;
    if (r.hitBottom && body.velocity.y > 0.0f) body.velocity.y = 0.0f;
    if (r.hitTop && body.velocity.y < 0.0f) body.velocity.y = 0.0f;
    if ((r.hitLeft && body.velocity.x < 0.0f) || (r.hitRight && body.velocity.x > 0.0f)) body.velocity.x = 0.0f;
    return r;
}

void resetMotion(KinematicBody* body, PlatformerController* platformer) {
    if (body != nullptr) *body = KinematicBody{};
    if (platformer != nullptr) {
        platformer->coyoteTimer = 0.0f;
        platformer->jumpBufferTimer = 0.0f;
        platformer->jumping = false;
    }
}

Rect tilemapBounds(const Scene& scene) {
    Rect bounds{};
    bool have = false;
    for (const Entity e : scene.entities()) {
        const auto* map = scene.tryGet<Tilemap>(e);
        if (map == nullptr || scene.tryGet<Transform>(e) == nullptr) continue;
        const Rect b = map->worldBounds(scene.worldPosition(e));
        bounds = have ? unite(bounds, b) : b;
        have = true;
    }
    return bounds;
}

void addTilemapCollision(const Scene& scene, physics::CollisionWorld& world) {
    // Tilemaps use their world position; rotation and scale do not apply to them.
    for (const Entity e : scene.entities()) {
        const auto* map = scene.tryGet<Tilemap>(e);
        if (map == nullptr || !map->solid || scene.tryGet<Transform>(e) == nullptr) continue;
        physics::SolidGrid grid;
        grid.origin = scene.worldPosition(e);
        grid.cellSize = map->tileSize;
        grid.width = map->width;
        grid.height = map->height;
        grid.solid.reserve(map->tiles.size());
        for (const int tile : map->tiles) grid.solid.push_back(tile >= 0 ? 1 : 0);
        world.addGrid(std::move(grid));
    }
}

void updateCameras(Scene& scene, const Rect& bounds, Vec2 viewSize) {
    auto& reg = scene.registry();
    reg.each<Camera>([&](Entity e, Camera& cam) {
        Vec2 target = cam.position;
        if (reg.tryGet<Transform>(e) != nullptr) target = scene.worldPosition(e);
        if (!cam.follow.isNil()) {
            const Entity followed = scene.findById(cam.follow);
            if (reg.tryGet<Transform>(followed) != nullptr) target = scene.worldPosition(followed);
        }
        if (cam.clampToBounds && !bounds.empty()) {
            const Vec2 half = viewSize * (0.5f / cam.zoom);
            const auto clampAxis = [](float v, float lo, float hi, float halfExtent) {
                if (hi - lo <= 2.0f * halfExtent) return (lo + hi) * 0.5f; // world smaller than view: centre
                return std::clamp(v, lo + halfExtent, hi - halfExtent);
            };
            target.x = clampAxis(target.x, bounds.left(), bounds.right(), half.x);
            target.y = clampAxis(target.y, bounds.top(), bounds.bottom(), half.y);
        }
        cam.position = target;
    });
}

} // namespace px::runtime
