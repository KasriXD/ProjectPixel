#pragma once

#include "px/net/PlayerStore.hpp"
#include "px/online/PlatformClient.hpp"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

// Saved player data kept by the platform, for game servers it started
// (docs/platform.md#saved-player-data). Requests run on a worker thread in
// order; saves are retried, and a newer save of the same player replaces
// one still waiting.
namespace px::online {

class PlatformPlayerStore final : public net::IPlayerStore {
public:
    // `client` must carry the server key.
    explicit PlatformPlayerStore(PlatformClient client);
    ~PlatformPlayerStore() override;

    void beginLoad(const std::string& userId) override;
    std::optional<Result<runtime::PlayerData>> takeLoaded(const std::string& userId) override;
    void save(const std::string& userId, const runtime::PlayerData& data, bool final) override;
    void release(const std::string& userId) override;
    void flush(double seconds) override;

private:
    struct Job {
        enum class Kind { Load, Save, Release } kind = Kind::Load;
        std::string userId;
        runtime::PlayerData data;
        bool final = false;
    };
    void run();

    PlatformClient m_client;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    std::deque<Job> m_jobs;
    std::map<std::string, Result<runtime::PlayerData>> m_loaded;
    bool m_busy = false;
    bool m_stop = false;
    std::thread m_worker;
};

} // namespace px::online
