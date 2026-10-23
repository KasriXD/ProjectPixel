#pragma once

#include "px/core/Guid.hpp"
#include "px/core/Result.hpp"
#include "px/physics/CollisionWorld.hpp"
#include "px/project/Project.hpp"
#include "px/runtime/Input.hpp"
#include "px/scene/Scene.hpp"
#include "px/scripting/ScriptHost.hpp"
#include "px/scripting/ScriptLibrary.hpp"
#include "px/scripting/ScriptSystem.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace px::runtime {

struct GameEvent {
    enum class Type {
        ScoreChanged,     // value = new score
        ObjectiveChanged, // text = objective
        LevelCompleted,   // text = completion message
        PlayerRespawned,  // entity = the player
        ScriptError,      // text = description
        ScriptEvent,      // text = "name: value" of a custom script event
    };
    Type type = Type::ScoreChanged;
    Guid entity;
    std::string text;
    int value = 0;
};

// Authoritative game state that is not part of the scene. In multiplayer
// this is what the server replicates to clients.
struct GameState {
    int score = 0;
    std::string objective;
    bool levelComplete = false;
    std::string completionMessage;
    int respawns = 0;
    int scriptErrors = 0;
    std::string lastScriptError; // for on-screen display in development builds
};

struct SessionOptions {
    scripting::ScriptLimits scriptLimits;
    std::uint64_t seed = 1; // deterministic randomness (math.random in scripts)
};

// One running instance of a game: a scene plus the systems that simulate it.
// Advances only in fixed ticks and has no graphics dependencies, so the same
// code runs in a client, in tests, and (later) on a dedicated server.
//
// Each tick runs these phases in order (docs/scripting.md#execution-order):
//   1. scripts: Start (new instances), then Tick
//   2. moving solids carry the bodies standing on them
//   3. platformer controllers, 4. kinematic movement
//   5. trigger enter/exit events to scripts
//   6. kill plane, 7. sprite animation, 8. cameras
// After each script phase, destroyed entities are removed and queued
// script events are delivered.
class GameSession final : private scripting::IScriptHost {
public:
    [[nodiscard]] static Result<std::unique_ptr<GameSession>> create(
        const project::ProjectManifest& project, scene::Scene scene,
        std::shared_ptr<const scripting::ScriptLibrary> scripts, const SessionOptions& options = {});
    ~GameSession() override;
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    // Advances the simulation by exactly one fixed step.
    void tick(const InputFrame& input);

    [[nodiscard]] scene::Scene& scene() { return m_scene; }
    [[nodiscard]] const scene::Scene& scene() const { return m_scene; }
    [[nodiscard]] const GameState& state() const { return m_state; }
    // Events raised during the most recent tick.
    [[nodiscard]] const std::vector<GameEvent>& events() const { return m_events; }
    [[nodiscard]] const ActionSet& actions() const { return m_actions; }
    [[nodiscard]] const physics::CollisionWorld& collisionWorld() const { return m_collision; }
    [[nodiscard]] scripting::ScriptSystem& scripts() { return *m_scripts; }

    [[nodiscard]] std::uint64_t tickCount() const override { return m_tick; }
    [[nodiscard]] float fixedDelta() const { return m_dt; }
    [[nodiscard]] double elapsedSeconds() const { return static_cast<double>(m_tick) * m_dt; }

    // First entity with a Player component, or kNullEntity.
    [[nodiscard]] scene::Entity player() const;
    // Union of all tilemap bounds (used for camera limits); empty if none.
    [[nodiscard]] const Rect& worldBounds() const { return m_worldBounds; }

private:
    GameSession(const project::ProjectManifest& project, scene::Scene scene);

    void buildStaticCollision();
    void flushPhase();
    void flushDestroyed();
    void updateMovingSolids();
    void updatePlatformers();
    void updateTopDown();
    void moveKinematicBodies();
    void updateTriggers();
    void updateKillPlane();
    void updateAnimations();
    void updateCameras();

    // IScriptHost
    int findAction(std::string_view name) const override;
    std::vector<std::string> actionNames() const override;
    bool actionHeld(int action) const override;
    bool actionPressed(int action) const override;
    void addScore(int amount) override;
    int score() const override { return m_state.score; }
    void setObjective(std::string text) override;
    std::string objective() const override { return m_state.objective; }
    void completeLevel(std::string message) override;
    bool respawnPlayer(scene::Entity entity) override;
    double timeSeconds() const override { return elapsedSeconds(); }
    void requestDestroy(scene::Entity entity) override;
    void scriptLog(log::Level level, std::string_view script, std::string_view message) override;
    void scriptError(const scripting::ScriptError& error) override;
    void scriptEvent(std::string_view name, std::string_view valueText) override;

    struct TriggerPair {
        scene::Entity trigger;
        scene::Entity other;
        bool operator==(const TriggerPair&) const = default;
    };
    struct SolidState {
        scene::Entity entity;
        Vec2 position;
    };

    scene::Scene m_scene;
    ActionSet m_actions;
    ActionId m_moveLeft = kInvalidAction;
    ActionId m_moveRight = kInvalidAction;
    ActionId m_jump = kInvalidAction;
    ActionId m_moveUp = kInvalidAction;
    ActionId m_moveDown = kInvalidAction;
    Vec2 m_viewSize;
    float m_dt;
    std::uint64_t m_tick = 0;
    InputFrame m_input;
    physics::CollisionWorld m_collision;
    Rect m_worldBounds{};
    GameState m_state;
    std::vector<GameEvent> m_events;
    std::unique_ptr<scripting::ScriptSystem> m_scripts;
    std::vector<scene::Entity> m_pendingDestroy;
    std::vector<TriggerPair> m_triggerPairs;    // overlapping now, in a stable order
    std::vector<SolidState> m_solids;           // solid boxes and their positions last tick
};

} // namespace px::runtime
