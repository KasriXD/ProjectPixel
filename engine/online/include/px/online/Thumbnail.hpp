#pragma once

#include "px/core/Result.hpp"

#include <cstdint>
#include <span>
#include <vector>

// Game thumbnails: any PNG, JPEG, BMP or TGA picture becomes a 320x180 PNG
// (cropped to 16:9 around the centre). The platform runs this in a separate,
// limited process (px-headless --check-thumbnail), like package checks.
namespace px::online {

inline constexpr int kThumbnailWidth = 320;
inline constexpr int kThumbnailHeight = 180;
inline constexpr std::size_t kMaxThumbnailUploadBytes = 4u << 20;

[[nodiscard]] Result<std::vector<std::uint8_t>> makeThumbnail(std::span<const std::uint8_t> picture);

} // namespace px::online
