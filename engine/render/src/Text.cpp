#include "px/render/Text.hpp"

#include "BuiltInFontFiles.hpp"

#include "px/core/Log.hpp"
#include "px/core/Utf8.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace px::render {

const project::FontData& builtInFontData() {
    static const project::FontData font = [] {
        const auto file = embedded::findBuiltInFontFile("pixel-default.font.json");
        if (!file) throw std::logic_error("the built-in font is not compiled in");
        auto parsed = project::parseFont(file->content, "built-in font");
        if (!parsed) throw std::logic_error(parsed.error().message);
        return std::move(parsed.value());
    }();
    return font;
}

// --- FontCache ---------------------------------------------------------------------------

FontCache::FontCache(IRenderer2D& renderer, TextureCache& textures, const IFileSystem& files,
                     const assets::AssetRegistry& assets)
    : m_renderer(renderer), m_textures(textures), m_files(files), m_assets(assets) {}

FontCache::~FontCache() { clear(); }

void FontCache::clear() {
    m_fonts.clear();
    m_failed.clear();
    if (m_builtInTexture != kNoTexture) {
        m_renderer.destroyTexture(m_builtInTexture);
        m_builtInTexture = kNoTexture;
    }
}

LoadedFont FontCache::builtIn() {
    if (m_builtInTexture == kNoTexture) {
        const auto file = embedded::findBuiltInFontFile("pixel-default.png");
        if (file) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(file->content.data());
            auto image = assets::decodeImage(std::span(bytes, file->content.size()), "built-in font");
            if (image) m_builtInTexture = m_renderer.createTexture(image.value(), assets::TextureFilter::Nearest);
        }
        if (m_builtInTexture == kNoTexture) return {&builtInFontData(), m_textures.missingTexture()};
    }
    return {&builtInFontData(), m_builtInTexture};
}

LoadedFont FontCache::get(assets::AssetId id) {
    if (id.isNil() || m_failed.contains(id)) return builtIn();
    auto it = m_fonts.find(id);
    if (it == m_fonts.end()) {
        auto font = project::loadFont(m_files, m_assets, id);
        if (!font) {
            log::error("Font {}: {} (using the built-in font)", id.toString(), font.error().message);
            m_failed.insert(id);
            return builtIn();
        }
        it = m_fonts.emplace(id, std::make_unique<project::FontData>(std::move(font.value()))).first;
    }
    return {it->second.get(), m_textures.get(it->second->texture)};
}

// --- layout ------------------------------------------------------------------------------

Vec2 anchorFactor(std::string_view anchor) {
    Vec2 f{0.0f, 0.0f};
    if (anchor == "center") return {0.5f, 0.5f};
    if (anchor.starts_with("bottom")) {
        f.y = 1.0f;
    } else if (!anchor.starts_with("top")) {
        f.y = 0.5f; // "left", "right"
    }
    if (anchor.ends_with("right")) {
        f.x = 1.0f;
    } else if (!anchor.ends_with("left")) {
        f.x = 0.5f; // "top", "bottom"
    }
    return f;
}

float alignFactor(std::string_view align) {
    if (align == "center") return 0.5f;
    if (align == "right") return 1.0f;
    return 0.0f;
}

namespace {

struct Letter {
    const project::Glyph* glyph; // nullptr: nothing to draw, advance only
    float advance;
    bool space;
};

struct Line {
    std::vector<Letter> letters;
    float width = 0.0f; // without trailing spaces
};

float trimmedWidth(const std::vector<Letter>& letters) {
    float width = 0.0f;
    float pending = 0.0f; // spaces not yet followed by a letter
    for (const Letter& l : letters) {
        if (l.space) {
            pending += l.advance;
        } else {
            width += pending + l.advance;
            pending = 0.0f;
        }
    }
    return width;
}

} // namespace

TextLayout layoutText(const project::FontData& font, std::string_view text, const TextLayoutOptions& options) {
    const project::Glyph* question = font.find(U'?');
    const project::Glyph* spaceGlyph = font.find(U' ');
    const float spaceAdvance = spaceGlyph != nullptr ? spaceGlyph->advance : static_cast<float>(font.size) * 0.5f;

    std::vector<Line> lines(1);
    std::size_t breakAt = 0; // in the current line: letters before the last space + 1 (0: none)
    float width = 0.0f;      // of the current line, with spaces
    const float wrap = options.wrapWidth > 0.0f ? options.wrapWidth : 0.0f;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const char32_t c = utf8::next(text, pos);
        if (c == U'\r') continue;
        if (c == U'\n') {
            lines.emplace_back();
            breakAt = 0;
            width = 0.0f;
            continue;
        }
        Letter letter{nullptr, 0.0f, false};
        if (c == U' ' || c == U'\t') {
            letter = {nullptr, c == U'\t' ? spaceAdvance * 4.0f : spaceAdvance, true};
        } else if (c < 0x20 || (c >= 0x7F && c < 0xA0)) {
            continue; // control characters
        } else {
            const project::Glyph* g = font.find(c);
            if (g == nullptr) g = question;
            if (g == nullptr) continue;
            letter = {g, g->advance, false};
        }
        Line* line = &lines.back();
        if (wrap > 0.0f && !letter.space && width + letter.advance > wrap && !line->letters.empty()) {
            // Break at the last space; a word longer than the line breaks where it is.
            std::vector<Letter> carried;
            if (breakAt > 0) {
                carried.assign(line->letters.begin() + static_cast<std::ptrdiff_t>(breakAt), line->letters.end());
                line->letters.resize(breakAt - 1); // drop the space
            }
            // Spaces right after the break do not start the next line.
            while (!carried.empty() && carried.front().space) carried.erase(carried.begin());
            lines.emplace_back();
            line = &lines.back();
            line->letters = std::move(carried);
            breakAt = 0;
            width = 0.0f;
            for (const Letter& l : line->letters) width += l.advance;
        }
        line->letters.push_back(letter);
        width += letter.advance;
        if (letter.space) breakAt = line->letters.size();
    }

    TextLayout layout;
    layout.lines = static_cast<int>(lines.size());
    float widest = 0.0f;
    for (Line& line : lines) {
        line.width = trimmedWidth(line.letters);
        widest = std::max(widest, line.width);
    }
    const float pad = std::max(options.padding, 0.0f);
    const float step = static_cast<float>(font.lineHeight) + options.lineSpacing;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        float x = pad + (widest - lines[i].width) * std::clamp(options.align, 0.0f, 1.0f);
        const float y = pad + static_cast<float>(i) * step;
        for (const Letter& l : lines[i].letters) {
            if (l.glyph != nullptr && l.glyph->source.w > 0.0f && l.glyph->source.h > 0.0f) {
                layout.glyphs.push_back({l.glyph->source, {x + l.glyph->offset.x, y + l.glyph->offset.y,
                                                           l.glyph->source.w, l.glyph->source.h}});
            }
            x += l.advance;
        }
    }
    const float textHeight = static_cast<float>(font.lineHeight) * static_cast<float>(lines.size()) +
                             options.lineSpacing * static_cast<float>(lines.size() - 1);
    layout.size = {std::round(widest + 2.0f * pad), std::round(std::max(textHeight, 0.0f) + 2.0f * pad)};
    return layout;
}

TextLayout layoutText(const project::FontData& font, const scene::Text& text) {
    TextLayoutOptions options;
    const float scale = text.scale > 0.0f ? text.scale : 1.0f;
    options.wrapWidth = text.wrapWidth > 0.0f ? text.wrapWidth / scale : 0.0f;
    options.lineSpacing = text.lineSpacing;
    options.padding = text.background.a > 0 ? text.padding : 0.0f;
    options.align = alignFactor(text.align);
    return layoutText(font, text.text, options);
}

void drawTextLayout(IRenderer2D& renderer, const LoadedFont& font, const TextLayout& layout, const scene::Text& text,
                    Vec2 topLeft, float pixelScale) {
    const Vec2 origin{std::round(topLeft.x), std::round(topLeft.y)};
    if (text.background.a > 0) {
        renderer.fillRect({origin.x, origin.y, layout.size.x * pixelScale, layout.size.y * pixelScale}, text.background);
    }
    const auto place = [&](const PlacedGlyph& g, float offset) {
        return Rect{origin.x + (g.dest.x + offset) * pixelScale, origin.y + (g.dest.y + offset) * pixelScale,
                    g.dest.w * pixelScale, g.dest.h * pixelScale};
    };
    if (text.shadow.a > 0) {
        for (const PlacedGlyph& g : layout.glyphs) {
            SpriteDraw d;
            d.texture = font.texture;
            d.source = g.source;
            d.dest = place(g, 1.0f);
            d.tint = text.shadow;
            renderer.drawSprite(d);
        }
    }
    for (const PlacedGlyph& g : layout.glyphs) {
        SpriteDraw d;
        d.texture = font.texture;
        d.source = g.source;
        d.dest = place(g, 0.0f);
        d.tint = text.color;
        renderer.drawSprite(d);
    }
}

} // namespace px::render
