#include "px/fontbake/FontBaker.hpp"

#include <stb_truetype.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace px::fontbake {

std::vector<CharRange> defaultRanges() {
    return {
        {0x20, 0x7E},   // ASCII
        {0xA0, 0xFF},   // Latin-1 Supplement
        {0x100, 0x17F}, // Latin Extended-A
        {0x370, 0x3FF}, // Greek and Coptic
        {0x400, 0x4FF}, // Cyrillic
        {0x2018, 0x201E}, // quotation marks
        {0x2022, 0x2026}, // bullet, ellipsis
        {0x20AC, 0x20AC}, // euro sign
        {0xFFFD, 0xFFFD}, // replacement character (shown for broken text)
    };
}

namespace {

struct Bitmap {
    char32_t codepoint = 0;
    int width = 0;
    int height = 0;
    int offsetX = 0;
    int offsetY = 0;
    int advance = 0;
    std::vector<std::uint8_t> alpha;
    int x = 0; // place in the image
    int y = 0;
};

// Trims rows and columns that are fully transparent, keeping the offset right.
void trim(Bitmap& b) {
    int left = b.width, right = -1, top = b.height, bottom = -1;
    for (int y = 0; y < b.height; ++y) {
        for (int x = 0; x < b.width; ++x) {
            if (b.alpha[static_cast<std::size_t>(y * b.width + x)] == 0) continue;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    if (right < 0) {
        b.width = b.height = 0;
        b.alpha.clear();
        return;
    }
    const int w = right - left + 1;
    const int h = bottom - top + 1;
    std::vector<std::uint8_t> out(static_cast<std::size_t>(w * h));
    for (int y = 0; y < h; ++y) {
        std::copy_n(b.alpha.begin() + (top + y) * b.width + left, w, out.begin() + y * w);
    }
    b.alpha = std::move(out);
    b.offsetX += left;
    b.offsetY += top;
    b.width = w;
    b.height = h;
}

// Shelf packing, tallest first. Returns the image height for `width`, or a
// height over `limit` if they do not fit.
int pack(std::vector<Bitmap*>& order, int width, int spacing, int limit) {
    int x = spacing, y = spacing, shelf = 0;
    for (Bitmap* b : order) {
        if (b->width == 0) continue;
        if (b->width + 2 * spacing > width) return limit + 1;
        if (x + b->width + spacing > width) {
            y += shelf + spacing;
            x = spacing;
            shelf = 0;
        }
        b->x = x;
        b->y = y;
        x += b->width + spacing;
        shelf = std::max(shelf, b->height);
        if (y + shelf + spacing > limit) return limit + 1;
    }
    return y + shelf + spacing;
}

std::uint32_t readU32(std::span<const std::uint8_t> d, std::size_t at) {
    return (std::uint32_t{d[at]} << 24) | (std::uint32_t{d[at + 1]} << 16) | (std::uint32_t{d[at + 2]} << 8) | d[at + 3];
}

// stb_truetype trusts the file's table directory. Checks that the directory
// and every table it lists are inside the file (a cut-off or damaged file
// then fails here instead of being read past its end).
bool tablesInside(std::span<const std::uint8_t> d) {
    std::size_t font = 0;
    if (d.size() >= 16 && readU32(d, 0) == 0x74746366u) font = readU32(d, 12); // "ttcf": the first font
    if (font > d.size() || d.size() - font < 12) return false;
    const std::size_t tables = (std::size_t{d[font + 4]} << 8) | d[font + 5];
    if (tables == 0 || (d.size() - font - 12) / 16 < tables) return false;
    for (std::size_t i = 0; i < tables; ++i) {
        const std::size_t record = font + 12 + i * 16;
        const std::uint64_t offset = readU32(d, record + 8);
        const std::uint64_t length = readU32(d, record + 12);
        if (offset + length > d.size()) return false;
    }
    return true;
}

std::string familyName(const stbtt_fontinfo& info) {
    // Name ID 1 (family), Windows Unicode English first, then Mac Roman.
    int length = 0;
    if (const char* name = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MICROSOFT,
                                                   STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1)) {
        std::string out;
        for (int i = 0; i + 1 < length; i += 2) {
            const auto c = static_cast<unsigned char>(name[i + 1]);
            if (name[i] == 0 && c >= 0x20 && c < 0x7F) out.push_back(static_cast<char>(c));
        }
        return out;
    }
    if (const char* name = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MAC, STBTT_MAC_EID_ROMAN,
                                                   STBTT_MAC_LANG_ENGLISH, 1)) {
        std::string out;
        for (int i = 0; i < length; ++i) {
            const auto c = static_cast<unsigned char>(name[i]);
            if (c >= 0x20 && c < 0x7F) out.push_back(static_cast<char>(c));
        }
        return out;
    }
    return {};
}

} // namespace

Result<BakedFont> bakeFont(std::span<const std::uint8_t> ttf, const BakeOptions& options) {
    if (options.pixelSize < 4 || options.pixelSize > 256) {
        return makeError(ErrorCode::InvalidArgument, "The size must be 4 to 256 pixels");
    }
    if (options.lineHeight < 0 || options.lineHeight > project::kMaxFontPixels) {
        return makeError(ErrorCode::InvalidArgument, "The line height must be at most {} pixels", project::kMaxFontPixels);
    }
    if (ttf.size() < 12 || ttf.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return makeError(ErrorCode::InvalidData, "This is not a font file");
    }
    const int offset = tablesInside(ttf) ? stbtt_GetFontOffsetForIndex(ttf.data(), 0) : -1;
    stbtt_fontinfo info;
    if (offset < 0 || stbtt_InitFont(&info, ttf.data(), offset) == 0) {
        return makeError(ErrorCode::InvalidData, "This is not a TrueType or OpenType font");
    }
    const float scale = stbtt_ScaleForMappingEmToPixels(&info, static_cast<float>(options.pixelSize));
    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
    BakedFont baked;
    baked.familyName = familyName(info);
    baked.font.size = options.pixelSize;
    baked.font.ascent = std::clamp(static_cast<int>(std::lround(static_cast<float>(ascent) * scale)), 0,
                                   project::kMaxFontPixels);
    baked.font.lineHeight =
        std::clamp(options.lineHeight > 0
                       ? options.lineHeight
                       : static_cast<int>(std::lround(static_cast<float>(ascent - descent + lineGap) * scale)),
                   1, project::kMaxFontPixels);


    std::vector<Bitmap> bitmaps;
    for (const auto& [first, last] : options.ranges) {
        for (char32_t c = first; c <= last && c >= first; ++c) {
            if (c >= 0xD800 && c <= 0xDFFF) continue;
            if (bitmaps.size() >= project::kMaxFontGlyphs) break;
            const int glyph = stbtt_FindGlyphIndex(&info, static_cast<int>(c));
            if (glyph == 0) {
                baked.missing.push_back(c);
                continue;
            }
            Bitmap b;
            b.codepoint = c;
            int advance = 0, bearing = 0;
            stbtt_GetGlyphHMetrics(&info, glyph, &advance, &bearing);
            b.advance = std::clamp(static_cast<int>(std::lround(static_cast<float>(advance) * scale)), 0,
                                   project::kMaxFontPixels);
            int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
            stbtt_GetGlyphBitmapBox(&info, glyph, scale, scale, &x0, &y0, &x1, &y1);
            b.width = std::clamp(x1 - x0, 0, project::kMaxFontPixels);
            b.height = std::clamp(y1 - y0, 0, project::kMaxFontPixels);
            b.offsetX = x0;
            b.offsetY = baked.font.ascent + y0;
            if (b.width > 0 && b.height > 0) {
                b.alpha.resize(static_cast<std::size_t>(b.width * b.height));
                stbtt_MakeGlyphBitmap(&info, b.alpha.data(), b.width, b.height, b.width, scale, scale, glyph);
                if (!options.smooth) {
                    for (auto& a : b.alpha) a = a >= options.threshold ? 255 : 0;
                }
                trim(b);
            } else {
                b.width = b.height = 0;
            }
            bitmaps.push_back(std::move(b));
        }
    }
    if (bitmaps.empty()) return makeError(ErrorCode::InvalidData, "The font has none of the characters asked for");

    // The narrowest square-ish image they fit in.
    std::vector<Bitmap*> order;
    for (Bitmap& b : bitmaps) order.push_back(&b);
    std::stable_sort(order.begin(), order.end(), [](const Bitmap* a, const Bitmap* b) { return a->height > b->height; });
    int width = 0, height = 0;
    for (int w = 64; w <= options.maxImageSize; w *= 2) {
        const int h = pack(order, w, options.spacing, options.maxImageSize);
        if (h <= options.maxImageSize && h <= w) {
            width = w;
            height = h;
            break;
        }
    }
    if (width == 0) {
        const int h = pack(order, options.maxImageSize, options.spacing, options.maxImageSize);
        if (h > options.maxImageSize) {
            return makeError(ErrorCode::LimitExceeded, "The letters do not fit in a {0}x{0} image: choose a smaller "
                                                       "size or fewer characters", options.maxImageSize);
        }
        width = options.maxImageSize;
        height = h;
    }

    baked.image.width = width;
    baked.image.height = std::max(height, 1);
    baked.image.pixels.assign(static_cast<std::size_t>(baked.image.width) * static_cast<std::size_t>(baked.image.height) * 4u, 0);
    for (const Bitmap& b : bitmaps) {
        project::Glyph g;
        g.codepoint = b.codepoint;
        g.advance = static_cast<float>(b.advance);
        if (b.width > 0) {
            g.source = {static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(b.width),
                        static_cast<float>(b.height)};
            g.offset = {static_cast<float>(std::clamp(b.offsetX, -project::kMaxFontPixels, project::kMaxFontPixels)),
                        static_cast<float>(std::clamp(b.offsetY, -project::kMaxFontPixels, project::kMaxFontPixels))};
            for (int y = 0; y < b.height; ++y) {
                for (int x = 0; x < b.width; ++x) {
                    auto* p = &baked.image.pixels[(static_cast<std::size_t>(b.y + y) * static_cast<std::size_t>(width) +
                                                   static_cast<std::size_t>(b.x + x)) * 4u];
                    p[0] = p[1] = p[2] = 255;
                    p[3] = b.alpha[static_cast<std::size_t>(y * b.width + x)];
                }
            }
        }
        baked.font.glyphs.push_back(g);
    }
    std::sort(baked.font.glyphs.begin(), baked.font.glyphs.end(),
              [](const project::Glyph& a, const project::Glyph& b) { return a.codepoint < b.codepoint; });
    baked.font.glyphs.erase(std::unique(baked.font.glyphs.begin(), baked.font.glyphs.end(),
                                        [](const project::Glyph& a, const project::Glyph& b) {
                                            return a.codepoint == b.codepoint;
                                        }),
                            baked.font.glyphs.end());
    return baked;
}

} // namespace px::fontbake
