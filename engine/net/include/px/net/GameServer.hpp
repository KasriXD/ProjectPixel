#pragma once

#include "px/core/Result.hpp"
#include "px/net/Protocol.hpp"
#include "px/net/Replication.hpp"
#include "px/net/Transport.hpp"
#include "px/runtime/GameLoader.hpp"
#include "px/runtime/GameSession.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

// The authoritative side of a multiplayer game (docs/multiplayer.md). It
// runs the only real simulation, fed with each player's input frames, and
// sends every client snapshots of the result. Clients never send state.
namespace px::net {

struct ServerOptions {
    std::string name = "Project Pixel server";
    std::size_t maxPlayers = 8;
    int snapshotInterval = 2;            // ticks between snapshots (2 = 30 per second at 60 ticks)
    double restartDelaySeconds = 8.0;    // after the level is complete; negative: never restart
    double helloTimeoutSeconds = 5.0;    // a connection must introduce itself within this time
    std::size_t inputBufferFrames = 2;   // frames a client's input is kept ahead, to absorb jitter
    std::size_t maxInputBufferFrames = 8; // the buffer grows up to this for clients with uneven links
    std::uint64_t seed = 1;              // session randomness (math.random)
};

struct ServerClientInfo {
    PeerId peer = kNoPeer;
    runtime::PlayerId player = runtime::kNoPlayer;
    std::string name;
    std::string address;
    PeerStats stats;
    std::size_t queuedInputs = 0;
    std::size_t inputBuffer = 0;   // frames currently kept ahead for this client
    std::uint64_t inputStalls = 0; // ticks the client's input had not arrived in time
};

class GameServer {
public:
    // `endpoint` must be a listening endpoint (it is owned by the server).
    [[nodiscard]] static Result<std::unique_ptr<GameServer>> create(const runtime::LoadedGame& game,
                                                                   std::unique_ptr<INetEndpoint> endpoint,
                                                                   const ServerOptions& options = {});
    ~GameServer();
    GameServer(const GameServer&) = delete;
    GameServer& operator=(const GameServer&) = delete;

    // One server tick: reads the network, runs one simulation tick with each
    // player's input, restarts the level when due, sends snapshots.
    void tick();
    // Tells every client the server is closing and disconnects them.
    void shutdown();

    [[nodiscard]] runtime::GameSession& session() { return *m_session; }
    [[nodiscard]] const runtime::GameSession& session() const { return *m_session; }
    [[nodiscard]] std::uint32_t epoch() const { return m_epoch; }
    [[nodiscard]] int tickRate() const { return m_tickRate; }
    [[nodiscard]] std::uint64_t contentHash() const { return m_contentHash; }
    [[nodiscard]] std::vector<ServerClientInfo> clients() const;
    [[nodiscard]] std::size_t playerCount() const;
    [[nodiscard]] std::uint64_t ticksRun() const { return m_ticksRun; }
    [[nodiscard]] std::size_t lastSnapshotBytes() const { return m_lastSnapshotBytes; }

private:
    struct Client {
        PeerId peer = kNoPeer;
        bool joined = false;
        std::uint64_t connectedAtTick = 0;
        runtime::PlayerId player = runtime::kNoPlayer;
        std::string name;
        std::map<std::uint32_t, runtime::InputFrame> inputs; // by sequence number
        std::uint32_t lastSequence = 0; // newest input used
        bool inputStarted = false;
        runtime::InputFrame lastFrame;
        std::uint32_t ackEpoch = 0;
        std::uint32_t ackTick = 0;
        std::uint64_t stalls = 0;
        std::size_t bufferTarget = 0;      // frames kept ahead; grows when input arrives unevenly
        std::uint64_t lastStallTick = 0;
    };

    GameServer(const runtime::LoadedGame& game, std::unique_ptr<INetEndpoint> endpoint, const ServerOptions& options);

    [[nodiscard]] Status startSession();
    void receive();
    void handleMessage(Client& client, const std::vector<std::uint8_t>& data);
    void handleHello(Client& client, const std::vector<std::uint8_t>& data);
    void handleInput(Client& client, const std::vector<std::uint8_t>& data);
    void reject(Client& client, DisconnectReason reason, const std::string& text);
    void dropClient(PeerId peer, const char* why);
    void applyInputs();
    void restartLevel();
    void sendSnapshots();
    [[nodiscard]] NetGameState gameState() const;
    [[nodiscard]] runtime::PlayerId allocatePlayerId();
    [[nodiscard]] std::string uniqueName(std::string wanted) const;

    runtime::LoadedGame m_game;
    std::unique_ptr<INetEndpoint> m_endpoint;
    ServerOptions m_options;
    int m_tickRate = 60;
    std::uint32_t m_actionMask = 0; // bits of the defined input actions
    std::unique_ptr<runtime::GameSession> m_session;
    NetIdMap m_ids;
    WorldState m_initial; // the prepared start scene (snapshot baseline 0)
    std::uint64_t m_contentHash = 0;
    std::deque<WorldState> m_history; // recent snapshots of this epoch, oldest first
    std::uint32_t m_epoch = 1;
    std::map<PeerId, Client> m_clients;
    runtime::PlayerId m_nextPlayer = 1;
    std::uint64_t m_ticksRun = 0;
    int m_restartCountdown = -1;
    std::size_t m_lastSnapshotBytes = 0;
    std::vector<NetEvent> m_events;
};

} // namespace px::net
