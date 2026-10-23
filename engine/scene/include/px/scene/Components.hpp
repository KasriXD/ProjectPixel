#pragma once

#include "px/assets/AssetId.hpp"
#include "px/core/Color.hpp"
#include "px/core/Guid.hpp"
#include "px/core/Math.hpp"
#include "px/scene/Registry.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Built-in component types. Components are plain data. Fields marked
// "runtime" are simulation state and are not written to scene files.
// The file representation of each component is documented in
// docs/project-format.md.
namespace px::scene {

// Every entity has exactly one EntityInfo (added by Scene::createEntity).
// The parent/child hierarchy is kept by Scene (Scene::parent, setParent).
struct EntityInfo {
    Guid id;          // stable identity, persisted; used for references and networking
    std::string name; // display name; not required to be unique
};

// Position, rotation and scale relative to the parent entity (or to the
// world for entities without a parent). Scene::worldTransform composes them.
struct Transform {
    Vec2 position{};       // world units (1 unit = 1 pixel at zoom 1), Y-down
    float rotation = 0.0f; // degrees, clockwise
    Vec2 scale{1.0f, 1.0f};
};

// Maps a point from the space described by `space` (e.g. a parent's world
// transform) to the space `space` itself is in: scale, rotate, translate.
[[nodiscard]] Vec2 transformPoint(const Transform& space, Vec2 local);
// The inverse of transformPoint. Axes with zero scale map to 0.
[[nodiscard]] Vec2 inverseTransformPoint(const Transform& space, Vec2 point);
// World transform of a child with transform `local` under a parent whose
// world transform is `parent`. Rotations add and scales multiply per axis
// (no shear: a rotated parent with non-uniform scale is approximated).
[[nodiscard]] Transform compose(const Transform& parent, const Transform& local);

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
    Entity ground;           // runtime: solid entity it stands on (null for tiles); used to carry riders
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

// Labels for finding and classifying entities from scripts ("coin", "enemy").
struct Tags {
    std::vector<std::string> values;

    [[nodiscard]] bool has(std::string_view tag) const {
        return std::find(values.begin(), values.end(), tag) != values.end();
    }
};

// A value a scene gives to a script property: number, boolean, string or 2D vector.
using ScriptValue = std::variant<double, bool, std::string, Vec2>;

struct ScriptProperty {
    std::string name;
    ScriptValue value;
};

// One Luau behaviour (a script asset) attached to an entity. `properties`
// override the defaults the script declares; the script defines the
// available names and their types. See docs/scripting.md.
struct AttachedScript {
    assets::AssetId script;
    std::vector<ScriptProperty> properties;

    [[nodiscard]] const ScriptProperty* findProperty(std::string_view name) const {
        for (const auto& p : properties) {
            if (p.name == name) return &p;
        }
        return nullptr;
    }
};

// The behaviours attached to an entity. Each runs as its own instance, in
// list order. (Scene format 2 allowed one, as the "Script" component.)
struct Scripts {
    std::vector<AttachedScript> items;
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
