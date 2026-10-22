#pragma once

#include "px/assets/AssetId.hpp"
#include "px/core/Color.hpp"
#include "px/core/Guid.hpp"
#include "px/core/Math.hpp"

#include <string>
#include <vector>

// Built-in component types. Components are plain data. Fields marked
// "runtime" are simulation state and are not written to scene files.
// The file representation of each component is documented in
// docs/project-format.md.
namespace px::scene {

// Every entity has exactly one EntityInfo (added by Scene::createEntity).
struct EntityInfo {
    Guid id;          // stable identity, persisted; used for references and networking
    std::string name; // display name; not required to be unique
};

struct Transform {
    Vec2 position{};       // world units (1 unit = 1 pixel at zoom 1), Y-down
    float rotation = 0.0f; // degrees, clockwise
    Vec2 scale{1.0f, 1.0f};
};

struct Sprite {
    assets::AssetId texture;
    Rect source{};             // texel rectangle; empty = whole texture
    Vec2 size{16.0f, 16.0f};   // world size before Transform.scale
    Vec2 pivot{0.5f, 0.5f};    // normalized point of the sprite placed at Transform.position
    Color tint = colors::white;
    int layer = 0;             // draw order: lower layers are drawn first
    bool flipX = false;
    bool flipY = false;
    bool visible = true;
};

// Frame-based animation that drives Sprite::source.
// Interim: frames live inline; animation clips become assets in Milestone 5.
struct SpriteAnimation {
    std::vector<Rect> frames;
    float framesPerSecond = 8.0f;
    bool loop = true;
    bool playing = true;
    float time = 0.0f; // runtime
};

// Grid of tiles drawn from a tileset texture. Transform.position is the
// top-left corner of tile (0, 0). Tile values index the tileset left-to-right,
// top-to-bottom; -1 means empty. When `solid`, every non-empty tile collides.
struct Tilemap {
    assets::AssetId tileset;
    Vec2 tileSize{16.0f, 16.0f};
    int tilesetColumns = 1;
    int width = 0;
    int height = 0;
    std::vector<int> tiles; // row-major, width * height entries
    bool solid = true;
    int layer = 0;
    Color tint = colors::white;

    [[nodiscard]] int tileAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) return -1;
        return tiles[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
    }
    [[nodiscard]] Rect worldBounds(Vec2 origin) const {
        return {origin.x, origin.y, tileSize.x * static_cast<float>(width), tileSize.y * static_cast<float>(height)};
    }
};

// Axis-aligned box centred on Transform.position + offset.
// Solid colliders block kinematic bodies; triggers only report overlaps.
struct BoxCollider {
    Vec2 size{16.0f, 16.0f};
    Vec2 offset{};
    bool isTrigger = false;

    [[nodiscard]] Rect worldRect(Vec2 position) const { return Rect::fromCenter(position + offset, size); }
};

// A body moved by gameplay code and stopped by solid geometry.
struct KinematicBody {
    Vec2 velocity{};         // runtime
    bool onGround = false;   // runtime
    bool hitCeiling = false; // runtime
    bool hitWall = false;    // runtime
};

// Reusable side-view character movement. Requires Transform, BoxCollider and
// KinematicBody. Reads the "move_left", "move_right" and "jump" actions.
struct PlatformerController {
    float maxSpeed = 110.0f;        // units/s
    float acceleration = 1200.0f;   // units/s^2 on the ground
    float deceleration = 1600.0f;   // units/s^2 on the ground with no input
    float airAcceleration = 900.0f; // units/s^2 in the air
    float jumpSpeed = 300.0f;       // initial upward speed, units/s
    float gravity = 900.0f;         // units/s^2
    float maxFallSpeed = 420.0f;    // units/s
    float jumpCutMultiplier = 0.45f; // upward speed kept when jump is released early
    float coyoteTime = 0.08f;       // s after leaving a ledge during which jumping still works
    float jumpBufferTime = 0.10f;   // s a jump press is remembered before landing

    float coyoteTimer = 0.0f;       // runtime
    float jumpBufferTimer = 0.0f;   // runtime
    bool jumping = false;           // runtime
};

// Picked up when a Player's collider overlaps this entity's trigger collider.
struct Collectible {
    std::string kind = "coin";
    int value = 1;
};

// Marks the locally or remotely controlled player character.
struct Player {
    Vec2 spawnPoint{};             // runtime: captured from Transform at session start
};

// Scene camera. Follows an entity (by stable id) if `follow` is set.
struct Camera {
    Guid follow;
    float zoom = 1.0f;
    bool clampToBounds = true;  // keep the view inside the union of tilemap bounds
    Vec2 position{};            // runtime: world point at the centre of the view
};

} // namespace px::scene
