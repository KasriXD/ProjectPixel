#pragma once

#include "px/core/Result.hpp"
#include "px/net/Protocol.hpp"
#include "px/net/Replication.hpp"
#include "px/net/Transport.hpp"
#include "px/physics/CollisionWorld.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/runtime/Movement.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>

// The client side of a multiplayer game (docs/multiplayer.md). It sends
// the player's input every tick and shows the world from the server's
// snapshots: other entities are drawn slightly in the past, blended between
// snapshots; the player's own character is predicted with the same movement
// code the server runs, and corrected smoothly when the server disagrees.
namespace px::net {

struct ClientOptions {
    std::string playerName;
    // How far in the past other entities are drawn. Negative: automatic
    // (two snapshot intervals plus two ticks, about 100 ms by default).
    double interpolationDelaySeconds = -1.0;
    bool predict = true;
    double connectTimeoutSeconds = 10.0;
};

struct ClientStats {
    double roundTripMs = 0.0;
    double packetLoss = 0.0;
    std::uint64_t bytesReceived = 0;
    std::uint64_t bytesSent = 0;
    std::uint64_t snapshots = 0;          // snapshots used
    std::uint64_t snapshotsDropped = 0;   // late, duplicated or without their baseline
    std::size_t lastSnapshotBytes = 0;
    std::uint64_t corrections = 0;        // times the prediction was off by more than a hair
    float lastCorrection = 0.0f;          // distance of the latest correction, world units
    std::size_t pendingInputs = 0;        // sent but not yet confirmed by the server
};

class GameClient {
public:
    enum class Status { Connecting, Joining, Playing, Disconnected };

    // `endpoint` is a client endpoint connecting to a server (owned by the client).
    [[nodiscard]] static Result<std::unique_ptr<GameClient>> create(const runtime::LoadedGame& game,
                                                                   std::unique_ptr<INetEndpoint> endpoint,
                                                                   const ClientOptions& options = {});
    ~GameClient();
    GameClient(const GameClient&) = delete;
    GameClient& operator=(const GameClient&) = delete;

    // Reads the network. `now` is a monotonic time in seconds.
    void receive(double now);
    // One client tick (1 / tickRate s): sends `input` and predicts the own
    // character. Does nothing until the server has let us in.
    void tick(const runtime::InputFrame& input);
    // Brings view() up to date for drawing at time `now`.
    void updateView(double now);
    // Says goodbye to the server.
    void disconnect();

    [[nodiscard]] Status status() const { return m_status; }
    // Why the client is disconnected (or what it is waiting for).
    [[nodiscard]] const std::string& statusText() const { return m_statusText; }
    [[nodiscard]] scene::Scene& view() { return m_view; }
    [[nodiscard]] const NetGameState& game() const { return m_game; }
    [[nodiscard]] runtime::PlayerId player() const { return m_player; }
    // The own character in view(), or kNullEntity.
    [[nodiscard]] scene::Entity character() const;
    [[nodiscard]] const runtime::ActionSet& actions() const { return m_actions; }
    [[nodiscard]] int tickRate() const { return m_tickRate; }
    [[nodiscard]] const std::string& serverName() const { return m_serverName; }
    [[nodiscard]] std::uint64_t contentHash() const { return m_contentHash; }
    [[nodiscard]] ClientStats stats() const;
    [[nodiscard]] const Rect& worldBounds() const { return m_worldBounds; }
    // Where the own character is predicted to be now (world), if known.
    [[nodiscard]] std::optional<Vec2> predictedPosition() const;

private:
    struct Predicted {
        Vec2 position;
        scene::KinematicBody body;
        scene::BoxCollider box;
        std::optional<scene::PlatformerController> platformer;
        std::optional<scene::TopDownController> topDown;
        Vec2 spawnPoint;
        bool flipX = false;
    };

    GameClient(const runtime::LoadedGame& game, std::unique_ptr<INetEndpoint> endpoint, const ClientOptions& options);

    void handleMessage(const std::vector<std::uint8_t>& data, double now);
    void handleSnapshot(const std::vector<std::uint8_t>& data, double now);
    void setDisconnected(std::string why);
    void sendInputs();
    void reconcile(const WorldState& newest, std::uint32_t lastInputSequence);
    void step(Predicted& p, const runtime::InputFrame& input) const;
    void rebuildCollision();
    [[nodiscard]] NetId ownNetId(const WorldState& state) const;
    [[nodiscard]] double serverTickAt(double now) const;

    runtime::LoadedGame m_loaded;
    std::unique_ptr<INetEndpoint> m_endpoint;
    ClientOptions m_options;
    runtime::ActionSet m_actions;
    runtime::CharacterActions m_characterActions;
    int m_tickRate = 60;
    int m_snapshotInterval = 2;
    Vec2 m_viewSize;

    Status m_status = Status::Connecting;
    std::string m_statusText = "Connecting...";
    double m_startTime = -1.0;
    PeerId m_server = kNoPeer;
    runtime::PlayerId m_player = runtime::kNoPlayer;
    std::string m_serverName;

    NetIdMap m_ids;
    WorldState m_initial;
    std::uint64_t m_contentHash = 0;
    std::uint32_t m_epoch = 0;
    std::map<std::uint32_t, WorldState> m_states; // received, by tick (this epoch)
    NetGameState m_game;

    scene::Scene m_view;   // what is drawn
    SceneReplica m_viewReplica;
    scene::Scene m_latest; // the newest snapshot, for prediction collision
    SceneReplica m_latestReplica;
    physics::CollisionWorld m_collision;
    std::uint64_t m_tilemapHash = 0;
    Rect m_worldBounds{};

    // Time: server ticks are estimated from snapshot arrival times.
    bool m_haveClock = false;
    double m_clockOffset = 0.0; // server tick = now * tickRate + offset
    double m_lastViewTime = -1.0;

    // Input and prediction.
    std::uint32_t m_sequence = 0;
    struct PendingInput {
        std::uint32_t sequence;
        runtime::InputFrame frame;
    };
    std::deque<PendingInput> m_pending;
    std::optional<Predicted> m_predicted;
    Vec2 m_smoothing{}; // visual offset that decays after a correction

    ClientStats m_stats;
    std::vector<NetEvent> m_events;
};

} // namespace px::net
