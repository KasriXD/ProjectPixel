#pragma once

#include "px/online/PlatformClient.hpp"
#include "px/runtime/Leaderboards.hpp"

#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

// Leaderboards kept by the platform, for game servers it started
// (docs/leaderboards.md). Scripts get answers at once from a copy in memory:
// the top scores and the bests of the players on this server, refreshed from
// the platform every few seconds. New bests are sent on a worker thread.
namespace px::online {

class PlatformLeaderboardStore final : public runtime::ILeaderboardStore {
public:
    // `client` must carry the server key; `boards` are the game's (project.json).
    PlatformLeaderboardStore(PlatformClient client, std::vector<project::LeaderboardDef> boards,
                             double refreshSeconds = 15.0);
    ~PlatformLeaderboardStore() override;

    bool submit(const project::LeaderboardDef& board, const std::string& account, const std::string& name,
                double score) override;
    std::optional<runtime::LeaderboardRow> entry(const project::LeaderboardDef& board,
                                                 const std::string& account) const override;
    std::vector<runtime::LeaderboardRow> top(const project::LeaderboardDef& board, std::size_t count) const override;
    void playerJoined(const std::string& account) override;
    void playerLeft(const std::string& account) override;

    // Waits until queued scores are sent and a refresh is done, at most `seconds` (tests, shutdown).
    void flush(double seconds);

private:
    void run();

    PlatformClient m_client;
    std::vector<project::LeaderboardDef> m_boards;
    double m_refreshSeconds;
    runtime::MemoryLeaderboardStore m_copy;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    std::map<std::pair<std::string, std::string>, PlatformClient::ScoreSubmission> m_pending; // (account, board)
    std::set<std::string> m_players;
    bool m_refreshWanted = true;
    bool m_busy = false;
    bool m_stop = false;
    std::thread m_worker;
};

} // namespace px::online
