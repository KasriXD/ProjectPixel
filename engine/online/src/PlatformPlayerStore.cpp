#include "px/online/PlatformPlayerStore.hpp"

#include "px/core/Log.hpp"

#include <chrono>

namespace px::online {

PlatformPlayerStore::PlatformPlayerStore(PlatformClient client) : m_client(std::move(client)) {
    m_client.setTimeoutSeconds(10);
    m_worker = std::thread([this] { run(); });
}

PlatformPlayerStore::~PlatformPlayerStore() {
    flush(10.0);
    {
        std::lock_guard lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_worker.joinable()) m_worker.join();
}

void PlatformPlayerStore::beginLoad(const std::string& userId) {
    std::lock_guard lock(m_mutex);
    m_loaded.erase(userId);
    m_jobs.push_back({Job::Kind::Load, userId, {}, false});
    m_wake.notify_one();
}

std::optional<Result<runtime::PlayerData>> PlatformPlayerStore::takeLoaded(const std::string& userId) {
    std::lock_guard lock(m_mutex);
    const auto it = m_loaded.find(userId);
    if (it == m_loaded.end()) return std::nullopt;
    auto out = std::move(it->second);
    m_loaded.erase(it);
    return out;
}

void PlatformPlayerStore::save(const std::string& userId, const runtime::PlayerData& data, bool final) {
    std::lock_guard lock(m_mutex);
    for (Job& j : m_jobs) {
        if (j.kind == Job::Kind::Save && j.userId == userId && !j.final) {
            j.data = data; // a newer copy replaces the waiting one
            j.final = final;
            return;
        }
    }
    m_jobs.push_back({Job::Kind::Save, userId, data, final});
    m_wake.notify_one();
}

void PlatformPlayerStore::release(const std::string& userId) {
    std::lock_guard lock(m_mutex);
    m_jobs.push_back({Job::Kind::Release, userId, {}, false});
    m_wake.notify_one();
}

void PlatformPlayerStore::flush(double seconds) {
    std::unique_lock lock(m_mutex);
    m_idle.wait_for(lock, std::chrono::duration<double>(seconds), [this] { return m_jobs.empty() && !m_busy; });
}

void PlatformPlayerStore::run() {
    while (true) {
        Job job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
            if (m_jobs.empty()) return; // stopping and nothing left
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
            m_busy = true;
        }
        if (job.kind == Job::Kind::Load) {
            Result<runtime::PlayerData> result = m_client.serverLoadData(job.userId);
            if (!result && result.error().code == ErrorCode::IoError) result = m_client.serverLoadData(job.userId);
            std::lock_guard lock(m_mutex);
            m_loaded.insert_or_assign(job.userId, std::move(result));
        } else {
            // Saves matter: a few tries before giving up (the platform keeps the
            // player's lock for a while, so nothing older can be loaded meanwhile).
            for (int attempt = 0; attempt < 4; ++attempt) {
                const Status done = job.kind == Job::Kind::Save ? m_client.serverSaveData(job.userId, job.data, job.final)
                                                                : m_client.serverRelease(job.userId);
                if (done) break;
                log::warn("Saving player {} failed: {}", job.userId, done.error().message);
                if (done.error().code != ErrorCode::IoError) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500 << attempt));
            }
        }
        {
            std::lock_guard lock(m_mutex);
            m_busy = false;
        }
        m_idle.notify_all();
    }
}

} // namespace px::online
