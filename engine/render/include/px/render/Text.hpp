#pragma once

#include "px/core/FileSystem.hpp"
#include "px/project/Font.hpp"
#include "px/render/Renderer2D.hpp"
#include "px/render/TextureCache.hpp"
#include "px/scene/Components.hpp"

#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Text with bitmap fonts (docs/text.md): laying out lines and drawing them.
namespace px::render {

// The texture asset id the built-in font's file names (it has no asset).
inline constexpr Guid kBuiltInFontTexture{0x707864656661'4c74ULL, 0x80666f6e74000001ULL};

struct LoadedFont {
    const project::FontData* data = nullptr;
    TextureHandle texture = kNoTexture;
};

// The built-in font ("Pixel Default", baked from Press Start 2P at 8 px).
[[nodiscard]] const project::FontData& builtInFontData();

// A game's fonts, read on first use. A font that cannot be loaded is reported
// once and the built-in one is used in its place.
class FontCache {
public:
    FontCache(IRenderer2D& renderer, TextureCache& textures, const IFileSystem& files,
              const assets::AssetRegistry& assets);
    ~FontCache();
    FontCache(const FontCache&) = delete;
    FontCache& operator=(const FontCache&) = delete;

    // nil: the built-in font.
    [[nodiscard]] LoadedFont get(assets::AssetId id);
    [[nodiscard]] LoadedFont builtIn();
    void clear();

private:
    IRenderer2D& m_renderer;
    TextureCache& m_textures;
    const IFileSystem& m_files;
    const assets::AssetRegistry& m_assets;
    std::unordered_map<assets::AssetId, std::unique_ptr<project::FontData>> m_fonts;
    std::unordered_set<assets::AssetId> m_failed;
    TextureHandle m_builtInTexture = kNoTexture;
};

// One letter placed in a text box (font pixels from the box's top-left).
struct PlacedGlyph {
    Rect source;
    Rect dest;
};

struct TextLayout {
    Vec2 size{};                    // of the box, padding included (font pixels)
    std::vector<PlacedGlyph> glyphs;
    int lines = 0;
};

struct TextLayoutOptions {
    float wrapWidth = 0.0f;   // font pixels; longer lines break at spaces (0: only at "\n")
    float lineSpacing = 0.0f; // extra font pixels between lines
    float padding = 0.0f;     // around the text
    float align = 0.0f;       // 0 left, 0.5 centred, 1 right
};

// Lays out UTF-8 text. Characters the font lacks are shown as "?" (or
// skipped if it has none); broken UTF-8 likewise.
[[nodiscard]] TextLayout layoutText(const project::FontData& font, std::string_view text,
                                    const TextLayoutOptions& options = {});

// A Text component's layout (its wrap width is in world or screen units, so
// it is divided by its scale here).
[[nodiscard]] TextLayout layoutText(const project::FontData& font, const scene::Text& text);

// 0..1 across and down for an anchor name ("top-left" .. "bottom-right").
[[nodiscard]] Vec2 anchorFactor(std::string_view anchor);
// 0, 0.5 or 1 for an align name.
[[nodiscard]] float alignFactor(std::string_view align);

// Draws a laid-out text with its top-left at `topLeft` (screen pixels),
// `pixelScale` screen pixels per font pixel, with the Text's colours.
void drawTextLayout(IRenderer2D& renderer, const LoadedFont& font, const TextLayout& layout, const scene::Text& text,
                    Vec2 topLeft, float pixelScale);

} // namespace px::render
