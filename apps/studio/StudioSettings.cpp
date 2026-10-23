#include "StudioSettings.hpp"

#include "Utf8.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <locale>
#include <sstream>
#include <string_view>

namespace px::studio {

namespace {

// Locale-independent. (std::from_chars for floats is unavailable in Apple's
// libc++ for the macOS versions we support.)
bool parseFloat(std::string_view text, float& out) {
    std::istringstream in{std::string(text)};
    in.imbue(std::locale::classic());
    float v = 0.0f;
    in >> v;
    if (in.fail() || !std::isfinite(v)) return false;
    in >> std::ws;
    if (!in.eof()) return false;
    out = v;
    return true;
}

} // namespace

void StudioSettings::addRecent(const std::filesystem::path& folder) {
    removeRecent(folder);
    std::vector<std::filesystem::path> updated{folder};
    for (auto& p : recentProjects) {
        if (updated.size() >= kMaxRecent) break;
        updated.push_back(std::move(p));
    }
    recentProjects = std::move(updated);
}

void StudioSettings::removeRecent(const std::filesystem::path& folder) {
    std::erase_if(recentProjects, [&](const std::filesystem::path& p) {
        return p.lexically_normal() == folder.lexically_normal();
    });
}

StudioSettings StudioSettings::load(const std::filesystem::path& file) {
    StudioSettings s;
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "recent" && !value.empty() && s.recentProjects.size() < kMaxRecent) {
            s.recentProjects.push_back(fromUtf8(value));
        } else if (key == "gridSize") {
            float v = 0.0f;
            if (parseFloat(value, v) && v >= 1.0f && v <= 1024.0f) s.gridSize = v;
        } else if (key == "uiScale") {
            float v = 0.0f;
            if (parseFloat(value, v) && v >= 0.5f && v <= 3.0f) s.uiScale = v;
        } else if (key == "snapToGrid") {
            s.snapToGrid = value == "1";
        } else if (key == "showGrid") {
            s.showGrid = value == "1";
        } else if (key == "showColliders") {
            s.showColliders = value == "1";
        } else if (key == "clearConsoleOnPlay") {
            s.clearConsoleOnPlay = value == "1";
        } else if (key == "layout") {
            if (value == "standard" || value == "classic") s.layout = value;
        } else if (key == "layoutVersion") {
            int v = 0;
            const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
            if (ec == std::errc() && ptr == value.data() + value.size()) s.layoutVersion = v;
        }
    }
    return s;
}

bool StudioSettings::save(const std::filesystem::path& file) const {
    std::ofstream out(file, std::ios::trunc);
    out.imbue(std::locale::classic());
    if (!out) return false;
    for (const auto& p : recentProjects) out << "recent=" << toUtf8(p) << '\n';
    out << "gridSize=" << gridSize << '\n';
    out << "uiScale=" << uiScale << '\n';
    out << "snapToGrid=" << (snapToGrid ? 1 : 0) << '\n';
    out << "showGrid=" << (showGrid ? 1 : 0) << '\n';
    out << "showColliders=" << (showColliders ? 1 : 0) << '\n';
    out << "clearConsoleOnPlay=" << (clearConsoleOnPlay ? 1 : 0) << '\n';
    out << "layout=" << layout << '\n';
    out << "layoutVersion=" << layoutVersion << '\n';
    return static_cast<bool>(out);
}

} // namespace px::studio
