#include "px/editor/LogBuffer.hpp"

namespace px::editor {

LogBuffer::LogBuffer(std::size_t capacity)
    : m_capacity(capacity == 0 ? 1 : capacity), m_start(std::chrono::steady_clock::now()) {
    m_sink = log::addSink([this](log::Level level, std::string_view message) { add(level, message); });
}

LogBuffer::~LogBuffer() { log::removeSink(m_sink); }

void LogBuffer::add(log::Level level, std::string_view text) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count();
    const std::lock_guard lock(m_mutex);
    ++m_counts[static_cast<std::size_t>(level)];
    ++m_revision;
    if (!m_entries.empty() && m_entries.back().level == level && m_entries.back().text == text) {
        ++m_entries.back().repeats;
        m_entries.back().seconds = seconds;
        return;
    }
    m_entries.push_back({level, std::string(text), seconds, 1});
    while (m_entries.size() > m_capacity) m_entries.pop_front();
}

void LogBuffer::clear() {
    const std::lock_guard lock(m_mutex);
    m_entries.clear();
    m_counts = {};
    ++m_revision;
}

std::vector<LogBuffer::Entry> LogBuffer::entries() const {
    const std::lock_guard lock(m_mutex);
    return {m_entries.begin(), m_entries.end()};
}

std::size_t LogBuffer::count(log::Level level) const {
    const std::lock_guard lock(m_mutex);
    return m_counts[static_cast<std::size_t>(level)];
}

std::uint64_t LogBuffer::revision() const {
    const std::lock_guard lock(m_mutex);
    return m_revision;
}

} // namespace px::editor
