#pragma once

#include "Process.hpp"
#include "px/core/Result.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

// The game servers the platform runs (docs/platform.md#game-servers): one
// px-server process per session, started when players need a seat and
// stopping by itself when everyone has left. The fleet also remembers which
// session each player is in, so a player's saved data is only ever loaded
// by one server at a time.
namespace px::platform {

struct FleetConfig {
    std::filesystem::path serverExe;  // px-server
    std::filesystem::path logDir;
    std::string platformUrl;          // how game servers reach the platform
    std::string publicHost = "127.0.0.1"; // how players reach game servers
    std::uint16_t firstPort = 27960;
    std::uint16_t portCount = 100;
    std::size_t maxServers = 20;
    std::size_t memoryLimitMiB = 1024;
    int cpuLimitPercent = 100;        // of one processor, over cpuGraceSeconds; 0: no limit
    double cpuSampleSeconds = 5.0;
    double cpuGraceSeconds = 30.0;
    bool sandbox = true;              // px-server --sandbox
    std::string platformKeyHex;       // public key servers check tokens with
    std::string tlsCert;              // given to servers for wss:// (browsers on https pages)
    std::string tlsKey;
    double readySeconds = 30.0;       // a new server must report ready within this
    double heartbeatSeconds = 30.0;   // and then say something at least this often
    double seatSeconds = 60.0;        // a reserved seat waits this long for its player
};

struct Seat {
    std::string session;
    std::string host;
    std::uint16_t port = 0;   // UDP, and TCP for WebSockets
    std::string webSocket;    // ws:// or wss:// URL of the same server
    std::string serverKey;    // the server's identity for encrypted traffic (net::encodeServerKey)
};

// Where a player is: the session holding them, while they play there or
// have a seat waiting.
struct PlayerLocation {
    std::string session;
    std::int64_t gameId = 0;
    int version = 0;
    bool playing = false; // in the server's last heartbeat (not only a seat)
};

struct SessionInfo {
    std::string id;
    std::int64_t gameId = 0;
    int version = 0;
    std::uint16_t port = 0;
    std::string state; // "starting", "ready", "ended"
    std::size_t players = 0;
    std::size_t reserved = 0;
    long long pid = 0;
    double cpuPercent = 0.0; // of one processor, over the last few seconds
};

// Decides when a game server has used too much processor time: more than
// `limitPercent` of one processor in every sample (taken at least
// `sampleSeconds` apart) for `graceSeconds`.
class CpuWatch {
public:
    CpuWatch(int limitPercent, double sampleSeconds, double graceSeconds)
        : m_limit(limitPercent), m_sampleSeconds(sampleSeconds), m_graceSeconds(graceSeconds) {}
    // `now` and `cpuSeconds` only grow. True once the server should stop.
    [[nodiscard]] bool sample(double now, double cpuSeconds);
    [[nodiscard]] double percent() const { return m_percent; }

private:
    int m_limit;
    double m_sampleSeconds;
    double m_graceSeconds;
    bool m_started = false;
    double m_at = 0.0;
    double m_cpu = 0.0;
    double m_percent = 0.0;
    double m_overSince = -1.0;
};

class Fleet {
public:
    explicit Fleet(FleetConfig config);
    ~Fleet();
    Fleet(const Fleet&) = delete;
    Fleet& operator=(const Fleet&) = delete;

    struct GameRef {
        std::int64_t gameId = 0;
        int version = 0;
        std::filesystem::path package;
        int maxPlayers = 8;
        std::string title;
    };
    // A seat for `userId` on a server running the game version: an existing
    // one with room and none of the `avoid` players (blocked either way), or
    // a new one (waits until it is ready). Errors are for the player.
    [[nodiscard]] Result<Seat> reserve(const GameRef& game, const std::string& userId,
                                       const std::set<std::string>& avoid = {});
    // A seat in one running session (joining a friend there). Errors are for the player.
    [[nodiscard]] Result<Seat> reserveIn(const std::string& session, const std::string& userId);
    [[nodiscard]] std::optional<PlayerLocation> whereIs(const std::string& userId) const;

    // --- Calls from game servers -----------------------------------------------------------
    [[nodiscard]] std::optional<std::string> sessionForKey(std::string_view serverKey) const;
    void ready(const std::string& session, std::uint16_t port);
    // Returns true if the server should stop.
    [[nodiscard]] bool heartbeat(const std::string& session, const std::vector<std::string>& players);
    void ended(const std::string& session);
    // Whether the server may read and write the player's saved data.
    [[nodiscard]] bool holdsPlayer(const std::string& session, const std::string& userId) const;
    void release(const std::string& session, const std::string& userId);
    [[nodiscard]] std::optional<std::int64_t> gameOf(const std::string& session) const;
    // The game version a session runs (0: unknown session).
    [[nodiscard]] int versionOf(const std::string& session) const;

    // --- Platform side ----------------------------------------------------------------------
    [[nodiscard]] int playing(std::int64_t gameId) const;
    [[nodiscard]] std::map<std::int64_t, int> playingByGame() const;
    // Asks every server of the game to stop (moderation).
    void stopGame(std::int64_t gameId);
    void stopAll();
    // Reaps ended processes, expires seats, kills silent servers. Call about once a second.
    void maintain();
    [[nodiscard]] std::vector<SessionInfo> sessions() const;

    // For /metrics: servers started, and how they ended ("finished",
    // "stopped", "failed", "exited", "cpu_limit").
    struct Counters {
        std::uint64_t started = 0;
        std::map<std::string, std::uint64_t> ended;
    };
    [[nodiscard]] Counters counters() const;

private:
    using Clock = std::chrono::steady_clock;
    struct Session {
        std::string id;
        std::string keyHash; // hex BLAKE2b of the server key
        std::string identity; // public half of the server's key for encrypted game traffic (base64url)
        GameRef game;
        std::uint16_t port = 0;
        enum class State { Starting, Ready, Ended } state = State::Starting;
        bool stopRequested = false;
        std::unique_ptr<Process> process;
        std::set<std::string> players;                     // in the last heartbeat
        std::map<std::string, Clock::time_point> reserved; // seats waiting for their player
        std::map<std::string, Clock::time_point> lastSeen;
        Clock::time_point started;
        Clock::time_point lastHeartbeat;
        std::optional<CpuWatch> cpu; // FleetConfig::cpuLimitPercent
    };

    [[nodiscard]] Result<Session*> startSession(const GameRef& game);
    // After a seat is reserved: waits for the server to be ready.
    [[nodiscard]] Result<Seat> seatWhenReady(std::unique_lock<std::mutex>& lock, const std::string& session,
                                             const std::string& userId);
    // Checks the player's lock: nullptr (free), their current session (a
    // reconnect to `wanted`), or an error.
    [[nodiscard]] Result<Session*> currentSessionFor(const std::string& userId, std::int64_t gameId, int version);
    [[nodiscard]] bool alive(const Session& s) const { return s.state != Session::State::Ended; }
    void endSession(Session& s, const char* why, const char* kind);
    [[nodiscard]] std::size_t occupancy(const Session& s) const;
    [[nodiscard]] Session* find(const std::string& id);
    [[nodiscard]] const Session* find(const std::string& id) const;

    FleetConfig m_config;
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::map<std::string, std::unique_ptr<Session>> m_sessions;
    Counters m_counters;
    std::map<std::string, std::string> m_userSession; // userId -> session id holding their data
};

} // namespace px::platform
