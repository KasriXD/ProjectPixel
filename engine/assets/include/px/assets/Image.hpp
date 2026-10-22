#pragma once

#include "px/core/Result.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace px::assets {

// Decoded image: 8-bit RGBA, rows top to bottom, no padding.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

// Uploaded assets are untrusted; dimensions are checked before decoding so a
// tiny file cannot request a huge allocation.
struct ImageLimits {
    int maxWidth = 8192;
    int maxHeight = 8192;
};

// Decodes PNG, JPEG, BMP or TGA bytes. sourceName is used in error messages.
[[nodiscard]] Result<Image> decodeImage(std::span<const std::uint8_t> bytes, std::string_view sourceName,
                                        ImageLimits limits = {});

} // namespace px::assets
