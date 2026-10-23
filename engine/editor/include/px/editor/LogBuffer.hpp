#pragma once

#include "px/core/Log.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace px::editor {

// Collects log messages for Studio's console: a px::log sink with a bounded
// history. Repeated identical messages are folded into one entry with a
// count, so a script error raised every tick does not flood the console.
class LogBuffer {
public:
    struct Entry {
        log::Level level = log::Level::Info;
        std::string text;
        double seconds = 0.0; // since the buffer was created
        int repeats = 1;
    };

    explicit LogBuffer(std::size_t capacity = 2000);
    ~LogBuffer();
    LogBuffer(const LogBuffer&) = delete;
    LogBuffer& operator=(const LogBuffer&) = delete;

    void add(log::Level level, std::string_view text); // also called by the sink
    void clear();

    [[nodiscard]] std::vector<Entry> entries() const;
    // Messages of this level since the last clear(), repeats included.
    [[nodiscard]] std::size_t count(log::Level level) const;
    // Changes whenever an entry is added or updated (for auto-scrolling).
    [[nodiscard]] std::uint64_t revision() const;

private:
    mutable std::mutex m_mutex;
    std::deque<Entry> m_entries;
    std::size_t m_capacity;
    std::array<std::size_t, 5> m_counts{};
    std::uint64_t m_revision = 0;
    std::chrono::steady_clock::time_point m_start;
    int m_sink = 0;
};

} // namespace px::editor
