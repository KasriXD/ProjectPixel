#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/core/FileSystem.hpp"
#include "px/core/Math.hpp"
#include "px/core/Result.hpp"
#include "px/project/Project.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Bitmap fonts ("*.font.json", docs/text.md#fonts): every letter as a
// rectangle of a texture asset, with where it goes and how far it moves the
// pen. Studio makes them from TrueType and OpenType fonts; games carry only
// these (the image is re-encoded like any other on the platform), so a
// player's device never parses a font file from the internet.
namespace px::project {

inline constexpr int kFontFormatVersion = 1;
inline constexpr std::size_t kMaxFontGlyphs = 20000;
inline constexpr int kMaxFontPixels = 1024; // size, line height and glyph sizes

struct Glyph {
    char32_t codepoint = 0;
    Rect source{};     // texels of the font's texture (empty for blank glyphs such as space)
    Vec2 offset{};     // from the pen at the top of the line to the top-left corner of `source`
    float advance = 0; // how far the pen moves right after it
};

struct FontData {
    assets::AssetId texture; // the letters (a texture asset)
    int size = 8;            // the pixel size it was made at
    int lineHeight = 10;     // from one line's top to the next
    int ascent = 8;          // from the top of a line to the baseline
    std::vector<Glyph> glyphs; // sorted by codepoint, no duplicates
    std::string source;      // what it was made from (shown in Studio, e.g. a font's name and licence)

    // The glyph of a character, or nullptr.
    [[nodiscard]] const Glyph* find(char32_t codepoint) const;
};

[[nodiscard]] Result<FontData> parseFont(std::string_view json, std::string_view sourceName,
                                         const assets::AssetRegistry* assets = nullptr);
[[nodiscard]] std::string serializeFont(const FontData& font);
[[nodiscard]] Result<FontData> loadFont(const IFileSystem& fs, const assets::AssetRegistry& assets, assets::AssetId id);

// Checks the glyphs against the texture's size (decoding fonts of uploaded games).
[[nodiscard]] Status checkFontTexture(const FontData& font, Vec2i textureSize, std::string_view sourceName);

} // namespace px::project
