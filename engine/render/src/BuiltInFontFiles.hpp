#pragma once

#include <optional>
#include <string_view>

// The built-in font, compiled in (the CMake build generates
// BuiltInFontFiles.generated.cpp from engine/render/fonts with cmake/EmbedFiles.cmake).
namespace px::render::embedded {

struct WebFile {
    std::string_view content;
    const char* contentType;
};

// "pixel-default.png", "pixel-default.font.json".
[[nodiscard]] std::optional<WebFile> findBuiltInFontFile(std::string_view name);

} // namespace px::render::embedded
