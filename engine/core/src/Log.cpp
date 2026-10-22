#include "px/core/Log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

namespace px::log {
namespace {

struct LogState {
    std::mutex mutex;
    std::vector<std::pair<int, Sink>> sinks;
    int nextHandle = 1;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

LogState& state() {
    static LogState s;
    return s;
}

std::atomic<Level> g_minLevel{Level::Info};
std::atomic<bool> g_console{true};

} // namespace

const char* levelName(Level level) noexcept {
    switch (level) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO";
        case Level::Warn: return "WARN";
        case Level::Error: return "ERROR";
    }
    return "?";
}

void setMinLevel(Level level) noexcept { g_minLevel.store(level); }
Level minLevel() noexcept { return g_minLevel.load(); }
bool enabled(Level level) noexcept { return level >= g_minLevel.load(); }
void setConsoleOutput(bool on) noexcept { g_console.store(on); }

int addSink(Sink sink) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    const int handle = s.nextHandle++;
    s.sinks.emplace_back(handle, std::move(sink));
    return handle;
}

void removeSink(int handle) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    std::erase_if(s.sinks, [handle](const auto& entry) { return entry.first == handle; });
}

void write(Level level, std::string_view message) {
    if (!enabled(level)) {
        return;
    }
    auto& s = state();
    std::lock_guard lock(s.mutex);
    if (g_console.load()) {
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - s.start).count();
        std::fprintf(stderr, "[%8.3f] %-5s %.*s\n", seconds, levelName(level),
                     static_cast<int>(message.size()), message.data());
    }
    for (const auto& [handle, sink] : s.sinks) {
        sink(level, message);
    }
}

} // namespace px::log
