#include "px/runtime/Leaderboards.hpp"

#include <algorithm>

namespace px::runtime {

std::vector<std::pair<std::string, const MemoryLeaderboardStore::Score*>> MemoryLeaderboardStore::sorted(
    const project::LeaderboardDef& board) const {
    std::vector<std::pair<std::string, const Score*>> out;
    const auto it = m_boards.find(board.id);
    if (it == m_boards.end()) return out;
    for (const auto& [account, score] : it->second) out.emplace_back(account, &score);
    std::sort(out.begin(), out.end(), [&board](const auto& a, const auto& b) {
        if (a.second->score != b.second->score) return board.better(a.second->score, b.second->score);
        return a.second->order < b.second->order;
    });
    return out;
}

bool MemoryLeaderboardStore::submit(const project::LeaderboardDef& board, const std::string& account,
                                    const std::string& name, double score) {
    std::lock_guard lock(m_mutex);
    auto& scores = m_boards[board.id];
    const auto it = scores.find(account);
    if (it != scores.end() && !board.better(score, it->second.score)) {
        it->second.name = name;
        return false;
    }
    scores[account] = Score{name, score, ++m_counter, 0};
    return true;
}

void MemoryLeaderboardStore::set(const project::LeaderboardDef& board, const std::string& account,
                                 const std::string& name, double score, int rank) {
    std::lock_guard lock(m_mutex);
    auto& scores = m_boards[board.id];
    const auto it = scores.find(account);
    if (it != scores.end() && board.better(it->second.score, score)) return; // a newer best than the platform knows
    if (it != scores.end() && it->second.score == score) {
        it->second.name = name;
        it->second.knownRank = rank;
        return;
    }
    scores[account] = Score{name, score, ++m_counter, rank};
}

std::optional<LeaderboardRow> MemoryLeaderboardStore::entry(const project::LeaderboardDef& board,
                                                            const std::string& account) const {
    std::lock_guard lock(m_mutex);
    const auto list = sorted(board);
    int rank = 0;
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (i == 0 || list[i].second->score != list[i - 1].second->score) rank = static_cast<int>(i) + 1;
        if (list[i].first != account) continue;
        // Outside the copy's top scores the platform's rank is more accurate.
        const int known = list[i].second->knownRank;
        return LeaderboardRow{known > rank ? known : rank, list[i].second->name, list[i].second->score};
    }
    return std::nullopt;
}

std::vector<LeaderboardRow> MemoryLeaderboardStore::top(const project::LeaderboardDef& board, std::size_t count) const {
    std::lock_guard lock(m_mutex);
    const auto list = sorted(board);
    std::vector<LeaderboardRow> out;
    int rank = 0;
    for (std::size_t i = 0; i < list.size() && out.size() < count; ++i) {
        if (i == 0 || list[i].second->score != list[i - 1].second->score) rank = static_cast<int>(i) + 1;
        out.push_back({rank, list[i].second->name, list[i].second->score});
    }
    return out;
}

std::size_t MemoryLeaderboardStore::size(const std::string& board) const {
    std::lock_guard lock(m_mutex);
    const auto it = m_boards.find(board);
    return it == m_boards.end() ? 0 : it->second.size();
}

} // namespace px::runtime
