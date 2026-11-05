#include "px/online/PlatformLeaderboardStore.hpp"

#include "px/core/Log.hpp"

#include <chrono>

namespace px::online {

PlatformLeaderboardStore::PlatformLeaderboardStore(PlatformClient client, std::vector<project::LeaderboardDef> boards,
                                                   double refreshSeconds)
    : m_client(std::move(client)), m_boards(std::move(boards)), m_refreshSeconds(refreshSeconds) {
    m_client.setTimeoutSeconds(10);
    if (!m_boards.empty()) m_worker = std::thread([this] { run(); });
}

PlatformLeaderboardStore::~PlatformLeaderboardStore() {
    flush(5.0);
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

bool PlatformLeaderboardStore::submit(const project::LeaderboardDef& board, const std::string& account,
                                      const std::string& name, double score) {
    const bool best = m_copy.submit(board, account, name, score);
    if (best) {
        std::lock_guard lock(m_mutex);
        m_pending[{account, board.id}] = PlatformClient::ScoreSubmission{account, board.id, score};
        m_wake.notify_one();
    }
    return best;
}

std::optional<runtime::LeaderboardRow> PlatformLeaderboardStore::entry(const project::LeaderboardDef& board,
                                                                       const std::string& account) const {
    return m_copy.entry(board, account);
}

std::vector<runtime::LeaderboardRow> PlatformLeaderboardStore::top(const project::LeaderboardDef& board,
                                                                   std::size_t count) const {
    return m_copy.top(board, count);
}

void PlatformLeaderboardStore::playerJoined(const std::string& account) {
    std::lock_guard lock(m_mutex);
    m_players.insert(account);
    m_refreshWanted = true; // fetch their bests now
    m_wake.notify_one();
}

void PlatformLeaderboardStore::playerLeft(const std::string& account) {
    std::lock_guard lock(m_mutex);
    m_players.erase(account);
}

void PlatformLeaderboardStore::flush(double seconds) {
    std::unique_lock lock(m_mutex);
    if (!m_worker.joinable()) return;
    m_refreshWanted = true;
    m_wake.notify_one();
    m_idle.wait_for(lock, std::chrono::duration<double>(seconds),
                    [this] { return m_pending.empty() && !m_refreshWanted && !m_busy; });
}

void PlatformLeaderboardStore::run() {
    using Clock = std::chrono::steady_clock;
    auto nextRefresh = Clock::now();
    while (true) {
        std::vector<PlatformClient::ScoreSubmission> scores;
        std::vector<std::string> players;
        bool refresh = false;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait_until(lock, nextRefresh,
                              [this] { return m_stop || !m_pending.empty() || m_refreshWanted; });
            if (m_stop && m_pending.empty()) return;
            for (auto& [key, s] : m_pending) scores.push_back(s);
            m_pending.clear();
            refresh = m_refreshWanted || Clock::now() >= nextRefresh || !scores.empty();
            m_refreshWanted = false;
            players.assign(m_players.begin(), m_players.end());
            m_busy = true;
        }
        if (!scores.empty()) {
            if (const Status sent = m_client.serverSubmitScores(scores); !sent) {
                log::warn("Sending leaderboard scores failed: {}", sent.error().message);
                if (sent.error().code == ErrorCode::IoError) {
                    std::lock_guard lock(m_mutex);
                    for (const auto& s : scores) m_pending.try_emplace({s.userId, s.board}, s); // try again later
                }
            }
        }
        if (refresh) {
            if (auto fetched = m_client.serverLeaderboards(100, players)) {
                for (const auto& view : fetched->boards) {
                    const project::LeaderboardDef* board = nullptr;
                    for (const auto& b : m_boards) {
                        if (b.id == view.id) board = &b;
                    }
                    if (board == nullptr) continue;
                    for (const auto& e : view.top) m_copy.set(*board, e.account, e.username, e.value, e.rank);
                    for (const auto& [account, entries] : fetched->players) {
                        const auto it = entries.find(board->id);
                        if (it != entries.end()) m_copy.set(*board, account, it->second.username, it->second.value, it->second.rank);
                    }
                }
            } else {
                log::warn("Reading leaderboards failed: {}", fetched.error().message);
            }
            nextRefresh = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                             std::chrono::duration<double>(m_refreshSeconds));
        }
        {
            std::lock_guard lock(m_mutex);
            m_busy = false;
        }
        m_idle.notify_all();
        // Bursts of new bests go out together, at most a few times a second.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

} // namespace px::online
