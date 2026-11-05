#pragma once

#include <optional>
#include <string_view>

// Pictures compiled into px-player for the launcher (the CMake build
// generates LauncherFiles.generated.cpp with cmake/EmbedFiles.cmake).
namespace px::player {

struct WebFile {
    std::string_view content;
    const char* contentType;
};

// "avatars.png": the 16 account portraits (services/platform/web/avatars.png).
[[nodiscard]] std::optional<WebFile> findLauncherFile(std::string_view name);

} // namespace px::player
