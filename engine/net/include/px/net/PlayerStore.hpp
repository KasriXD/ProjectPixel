#pragma once

#include "px/core/Result.hpp"
#include "px/runtime/PlayerData.hpp"

#include <map>
#include <optional>
#include <string>

// Where a game server's players' saved data comes from and goes
// (docs/platform.md#saved-player-data). The platform's implementation talks
// HTTP on a worker thread (online::PlatformPlayerStore); GameServer only
// polls, so a slow platform never stalls the simulation.
namespace px::net {

class IPlayerStore {
public:
    virtual ~IPlayerStore() = default;
    // Starts loading a player's data (they are joining).
    virtual void beginLoad(const std::string& userId) = 0;
    // The loaded data once it is there (and forgets it); nullopt while loading.
    [[nodiscard]] virtual std::optional<Result<runtime::PlayerData>> takeLoaded(const std::string& userId) = 0;
    // Saves a player's data. `final`: they left; the platform may send them elsewhere afterwards.
    virtual void save(const std::string& userId, const runtime::PlayerData& data, bool final) = 0;
    // The player left before their data was loaded: nothing to save.
    virtual void release(const std::string& userId) = 0;
    // Waits until queued saves are done (shutdown), at most `seconds`.
    virtual void flush(double seconds) = 0;
};

// In memory, answering at once: tests and local servers.
class MemoryPlayerStore final : public IPlayerStore {
public:
    void beginLoad(const std::string& userId) override { m_loading[userId] = true; }
    std::optional<Result<runtime::PlayerData>> takeLoaded(const std::string& userId) override {
        if (m_loading.erase(userId) == 0) return std::nullopt;
        const auto it = m_saved.find(userId);
        return Result<runtime::PlayerData>(it == m_saved.end() ? runtime::PlayerData{} : it->second);
    }
    void save(const std::string& userId, const runtime::PlayerData& data, bool final) override {
        m_saved[userId] = data;
        ++saves;
        if (final) ++finalSaves;
    }
    void release(const std::string&) override { ++releases; }
    void flush(double) override {}

    std::map<std::string, runtime::PlayerData> m_saved;
    int saves = 0;
    int finalSaves = 0;
    int releases = 0;

private:
    std::map<std::string, bool> m_loading;
};

} // namespace px::net
