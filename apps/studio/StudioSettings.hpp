#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace px::studio {

// Per-user Studio preferences, stored as simple "key=value" lines in the
// user's preference folder (SDL_GetPrefPath), next to Dear ImGui's window
// layout file.
struct StudioSettings {
    std::vector<std::filesystem::path> recentProjects; // most recent first
    float gridSize = 16.0f;
    bool snapToGrid = true;
    bool showGrid = true;
    bool showColliders = true;
    bool clearConsoleOnPlay = true;
    float uiScale = 1.0f; // multiplied with the monitor's scale

    static constexpr std::size_t kMaxRecent = 10;

    void addRecent(const std::filesystem::path& folder);
    void removeRecent(const std::filesystem::path& folder);

    [[nodiscard]] static StudioSettings load(const std::filesystem::path& file);
    [[nodiscard]] bool save(const std::filesystem::path& file) const;
};

} // namespace px::studio
