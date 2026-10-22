#pragma once

#include "px/core/Guid.hpp"
#include "px/physics/CollisionWorld.hpp"
#include "px/project/Project.hpp"
#include "px/runtime/Input.hpp"
#include "px/scene/Scene.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace px::runtime {

struct GameEvent {
    enum class Type {
        Collected,       // a player picked up a Collectible (kind, value)
        PlayerRespawned, // a player fell below the kill plane
        AllCollected,    // every collectible in the scene has been picked up
    };
    Type type = Type::Collected;
    Guid entity;      // the collectible or player involved, if any
    std::string kind; // collectible kind
    int value = 0;
};

struct GameState {
    int score = 0;
    int collectiblesTotal = 0;
    int collectiblesCollected = 0;
    int respawns = 0;
    bool allCollected = false;
};

// One running instance of a game: a scene plus the systems that simulate it.
// Advances only in fixed ticks and has no graphics dependencies, so the same
// code can run in a client, in tests, and on a dedicated server.
class GameSession {
public:
    GameSession(const project::ProjectManifest& project, scene::Scene scene);

    // Advances the simulation by exactly one fixed step.
    void tick(const InputFrame& input);

    [[nodiscard]] scene::Scene& scene() { return m_scene; }
    [[nodiscard]] const scene::Scene& scene() const { return m_scene; }
    [[nodiscard]] const GameState& state() const { return m_state; }
    // Events raised during the most recent tick.
    [[nodiscard]] const std::vector<GameEvent>& events() const { return m_events; }
    [[nodiscard]] const ActionSet& actions() const { return m_actions; }
    [[nodiscard]] const physics::CollisionWorld& collisionWorld() const { return m_collision; }

    [[nodiscard]] std::uint64_t tickCount() const { return m_tick; }
    [[nodiscard]] float fixedDelta() const { return m_dt; }
    [[nodiscard]] double elapsedSeconds() const { return static_cast<double>(m_tick) * m_dt; }

    // First entity with a Player component, or kNullEntity.
    [[nodiscard]] scene::Entity player() const;
    // Union of all tilemap bounds (used for camera limits); empty if none.
    [[nodiscard]] const Rect& worldBounds() const { return m_worldBounds; }

private:
    void buildStaticCollision();
    void updatePlatformers(const InputFrame& input);
    void moveKinematicBodies();
    void updateCollectibles();
    void updateKillPlane();
    void updateAnimations();
    void updateCameras();

    scene::Scene m_scene;
    ActionSet m_actions;
    ActionId m_moveLeft = kInvalidAction;
    ActionId m_moveRight = kInvalidAction;
    ActionId m_jump = kInvalidAction;
    Vec2 m_viewSize;
    float m_dt;
    std::uint64_t m_tick = 0;
    physics::CollisionWorld m_collision;
    Rect m_worldBounds{};
    GameState m_state;
    std::vector<GameEvent> m_events;
};

} // namespace px::runtime
