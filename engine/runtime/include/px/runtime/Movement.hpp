#pragma once

#include "px/core/Math.hpp"
#include "px/physics/CollisionWorld.hpp"
#include "px/runtime/Input.hpp"
#include "px/scene/Components.hpp"
#include "px/scene/Scene.hpp"

// Character movement and cameras as plain functions, shared by the
// authoritative simulation (GameSession) and by network clients, which
// predict their own character with exactly the same code.
namespace px::runtime {

// The standard actions the built-in controllers read, resolved once.
struct CharacterActions {
    ActionId left = kInvalidAction;
    ActionId right = kInvalidAction;
    ActionId up = kInvalidAction;
    ActionId down = kInvalidAction;
    ActionId jump = kInvalidAction;

    [[nodiscard]] static CharacterActions from(const ActionSet& actions);
};

// One tick of side-view control: running, gravity, jumps (with coyote time,
// jump buffering and jump cut). Mirrors the sprite when moving left.
void stepPlatformerControl(scene::PlatformerController& c, scene::KinematicBody& body, scene::Sprite* sprite,
                           const InputFrame& input, const CharacterActions& actions, float dt);

// One tick of top-down control: eight directions at the same speed, no gravity.
void stepTopDownControl(scene::TopDownController& c, scene::KinematicBody& body, scene::Sprite* sprite,
                        const InputFrame& input, const CharacterActions& actions, float dt);

// Moves a body's collider box (in world space) by velocity * dt through the
// collision world, X then Y. Updates the contact flags and stops velocity
// into whatever it hit. Returns the result (displacement and what it stands on).
physics::MoveResult moveBody(const physics::CollisionWorld& world, scene::KinematicBody& body, const Rect& box,
                             float dt);

// Clears motion state after a teleport (respawn).
void resetMotion(scene::KinematicBody* body, scene::PlatformerController* platformer);

// Union of the world bounds of all tilemaps (empty if there are none).
[[nodiscard]] Rect tilemapBounds(const scene::Scene& scene);

// Adds the solid tiles of every solid tilemap to `world` as grids.
void addTilemapCollision(const scene::Scene& scene, physics::CollisionWorld& world);

// Moves every Camera to its target: the followed entity (or the camera's own
// position), kept inside `bounds` when clampToBounds is set.
void updateCameras(scene::Scene& scene, const Rect& bounds, Vec2 viewSize);

} // namespace px::runtime
