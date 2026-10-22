#pragma once

#include <cstdint>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

// Minimal thread-safe logging. Messages go to stderr (optional) and to any
// registered sinks, e.g. a future Studio console or a test capture.
namespace px::log {

enum class Level : std::uint8_t { Trace, Debug, Info, Warn, Error };

using Sink = std::function<void(Level, std::string_view message)>;

[[nodiscard]] const char* levelName(Level level) noexcept;

void setMinLevel(Level level) noexcept;
[[nodiscard]] Level minLevel() noexcept;
[[nodiscard]] bool enabled(Level level) noexcept;

void setConsoleOutput(bool enabled) noexcept;

// Returns a handle for removeSink().
int addSink(Sink sink);
void removeSink(int handle);

void write(Level level, std::string_view message);

template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Trace)) write(Level::Trace, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void debug(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Debug)) write(Level::Debug, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Info)) write(Level::Info, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Warn)) write(Level::Warn, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Error)) write(Level::Error, std::format(fmt, std::forward<Args>(args)...));
}

} // namespace px::log
