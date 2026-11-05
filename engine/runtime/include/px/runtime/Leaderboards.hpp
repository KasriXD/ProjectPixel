#pragma once

#include "px/project/Project.hpp"
#include "px/scripting/ScriptHost.hpp"

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// Where a session's leaderboard scores go (docs/leaderboards.md). Scripts
// ask synchronously, so every store answers from memory: the platform's
// (online::PlatformLeaderboardStore) keeps a copy it refreshes and sends
// new bests on a worker thread.
namespace px::runtime {

using LeaderboardRow = scripting::LeaderboardRow;

class ILeaderboardStore {
public:
    virtual ~ILeaderboardStore() = default;
    // Records a score for an account (a platform account id, or a name when
    // playing locally); the store keeps each account's best. Returns true
    // when it is their new best.
    virtual bool submit(const project::LeaderboardDef& board, const std::string& account, const std::string& name,
                        double score) = 0;
    [[nodiscard]] virtual std::optional<LeaderboardRow> entry(const project::LeaderboardDef& board,
                                                              const std::string& account) const = 0;
    // Best first.
    [[nodiscard]] virtual std::vector<LeaderboardRow> top(const project::LeaderboardDef& board,
                                                          std::size_t count) const = 0;
    // Players arriving and leaving (a store that fetches from elsewhere
    // prepares their entries).
    virtual void playerJoined(const std::string& /*account*/) {}
    virtual void playerLeft(const std::string& /*account*/) {}
};

// In memory: playing alone, servers without a platform, tests; also the
// copy the platform's store answers from. Thread-safe.
class MemoryLeaderboardStore : public ILeaderboardStore {
public:
    bool submit(const project::LeaderboardDef& board, const std::string& account, const std::string& name,
                double score) override;
    [[nodiscard]] std::optional<LeaderboardRow> entry(const project::LeaderboardDef& board,
                                                      const std::string& account) const override;
    [[nodiscard]] std::vector<LeaderboardRow> top(const project::LeaderboardDef& board, std::size_t count) const override;

    // Sets an account's score as the platform has it (keeps the better of
    // this and a newer local one). `rank`: the platform's rank, used when
    // the account is not among the scores this copy has.
    void set(const project::LeaderboardDef& board, const std::string& account, const std::string& name, double score,
             int rank = 0);
    [[nodiscard]] std::size_t size(const std::string& board) const;

private:
    struct Score {
        std::string name;
        double score = 0.0;
        std::uint64_t order = 0; // earlier scores win ties
        int knownRank = 0;
    };
    // Best first, equal scores in the order they were reached.
    [[nodiscard]] std::vector<std::pair<std::string, const Score*>> sorted(const project::LeaderboardDef& board) const;

    mutable std::mutex m_mutex;
    std::map<std::string, std::map<std::string, Score>> m_boards; // board -> account -> best
    std::uint64_t m_counter = 0;
};

} // namespace px::runtime
