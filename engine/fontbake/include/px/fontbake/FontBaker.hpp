#pragma once

#include "px/assets/Image.hpp"
#include "px/core/Result.hpp"
#include "px/project/Font.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace px::fontbake {

// Ranges of characters to bake (inclusive).
using CharRange = std::pair<char32_t, char32_t>;

// Latin (ASCII, Latin-1, Latin Extended-A), Greek and Cyrillic.
[[nodiscard]] std::vector<CharRange> defaultRanges();

struct BakeOptions {
    int pixelSize = 16;      // the em square in pixels; pixel fonts look right at their design size
    std::vector<CharRange> ranges = defaultRanges();
    // Pixel fonts: every pixel fully on or off (no blur when scaled up).
    // Smooth: anti-aliased edges, for fonts drawn at their own size.
    bool smooth = false;
    int threshold = 128;     // pixel mode: coverage (0..255) at which a pixel is on
    int lineHeight = 0;      // from one line's top to the next (0: as the font says)
    int spacing = 1;         // empty pixels between letters in the image
    int maxImageSize = 2048;
};

struct BakedFont {
    project::FontData font; // font.texture is left nil: the caller makes the image a texture asset
    assets::Image image;    // white letters on transparent
    std::vector<char32_t> missing; // asked for but not in the font
    std::string familyName; // as the font names itself (may be empty)
};

// Bakes `ttf` (TrueType or OpenType, the first font of
// a collection). Only for files the user chose: not hardened against hostile data.
[[nodiscard]] Result<BakedFont> bakeFont(std::span<const std::uint8_t> ttf, const BakeOptions& options = {});

} // namespace px::fontbake
