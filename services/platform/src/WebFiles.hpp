#pragma once

#include <optional>
#include <string_view>

// The platform's own web page for playing in a browser (web/play.*),
// compiled into px-platform (the CMake build generates WebFiles.generated.cpp).
namespace px::platform {

struct WebFile {
    std::string_view content;
    const char* contentType;
};

// "play.html", "play.js", "play.css", "avatars.png" (the account
// portraits, also compiled into px-player); nothing for other names.
[[nodiscard]] std::optional<WebFile> findWebFile(std::string_view name);

} // namespace px::platform
