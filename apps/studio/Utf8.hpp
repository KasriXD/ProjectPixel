#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace px::studio {

// Paths shown in the UI and stored in settings are UTF-8 on every platform
// (std::filesystem::path uses UTF-16 internally on Windows).
inline std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string s = path.u8string();
    return {s.begin(), s.end()};
}

inline std::filesystem::path fromUtf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

} // namespace px::studio
