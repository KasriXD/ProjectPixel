#pragma once

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

// Token buckets per key ("login:203.0.113.5"): `perWindow` actions per
// `windowSeconds`, refilled continuously. In memory: a restart forgets them.
namespace px::platform {

class RateLimiter {
public:
    bool allow(const std::string& key, double perWindow, double windowSeconds) {
        if (!m_enabled) return true;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(m_mutex);
        if (++m_calls % 4096 == 0) cleanup(now);
        auto [it, inserted] = m_buckets.try_emplace(key, Bucket{perWindow, now});
        Bucket& b = it->second;
        if (!inserted) {
            const double elapsed = std::chrono::duration<double>(now - b.updated).count();
            b.tokens = std::min(perWindow, b.tokens + elapsed * perWindow / windowSeconds);
            b.updated = now;
        }
        if (b.tokens < 1.0) return false;
        b.tokens -= 1.0;
        return true;
    }
    void setEnabled(bool enabled) { m_enabled = enabled; }

private:
    struct Bucket {
        double tokens;
        std::chrono::steady_clock::time_point updated;
    };
    void cleanup(std::chrono::steady_clock::time_point now) {
        for (auto it = m_buckets.begin(); it != m_buckets.end();) {
            it = now - it->second.updated > std::chrono::hours(2) ? m_buckets.erase(it) : std::next(it);
        }
    }
    std::mutex m_mutex;
    std::map<std::string, Bucket> m_buckets;
    unsigned long long m_calls = 0;
    bool m_enabled = true;
};

} // namespace px::platform
