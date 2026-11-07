#include "px/project/Font.hpp"

#include "JsonUtil.hpp"

#include <algorithm>
#include <cmath>

namespace px::project {

using detail::fail;
using detail::Json;
using detail::ObjectReader;

const Glyph* FontData::find(char32_t codepoint) const {
    const auto it = std::lower_bound(glyphs.begin(), glyphs.end(), codepoint,
                                     [](const Glyph& g, char32_t c) { return g.codepoint < c; });
    return it != glyphs.end() && it->codepoint == codepoint ? &*it : nullptr;
}

namespace {

int readInt(const Json& j, const std::string& path, int lo, int hi) {
    if (!j.is_number_integer()) fail(path, "expected a whole number");
    const auto v = j.get<std::int64_t>();
    if (v < lo || v > hi) fail(path, std::format("must be {} to {}", lo, hi));
    return static_cast<int>(v);
}

bool validCodepoint(std::int64_t c) { return c >= 0 && c <= 0x10FFFF && !(c >= 0xD800 && c <= 0xDFFF); }

} // namespace

Result<FontData> parseFont(std::string_view json, std::string_view sourceName, const assets::AssetRegistry* assets) {
    try {
        Json root = detail::parseJsonText(json);
        std::vector<std::string> warnings;
        detail::checkAndMigrate(root, "px.font", kFontFormatVersion, {}, "font", warnings);
        ObjectReader r(root, "");
        (void)r.find("format");
        (void)r.find("formatVersion");
        (void)r.find("source"); // where it was made from (informational)
        FontData font;
        r.required("texture", font.texture);
        if (font.texture.isNil()) fail("texture", "a texture asset id is required");
        if (assets != nullptr) {
            const auto* entry = assets->find(font.texture);
            if (entry == nullptr) {
                fail("texture", std::format("refers to asset {}, which is not in the project", font.texture.toString()));
            }
            if (entry->type != assets::AssetType::Texture) {
                fail("texture",
                     std::format("asset '{}' is a {}, not a texture", entry->name, assets::assetTypeName(entry->type)));
            }
        }
        font.size = readInt(r.require("size"), "size", 1, kMaxFontPixels);
        font.lineHeight = readInt(r.require("lineHeight"), "lineHeight", 1, kMaxFontPixels);
        font.ascent = readInt(r.require("ascent"), "ascent", 0, kMaxFontPixels);
        const Json& glyphs = r.require("glyphs");
        if (!glyphs.is_array()) fail("glyphs", "expected an array of [codepoint, x, y, w, h, offsetX, offsetY, advance]");
        if (glyphs.size() > kMaxFontGlyphs) fail("glyphs", std::format("at most {} glyphs", kMaxFontGlyphs));
        font.glyphs.reserve(glyphs.size());
        for (std::size_t i = 0; i < glyphs.size(); ++i) {
            const std::string path = detail::indexPath("glyphs", i);
            const Json& g = glyphs[i];
            if (!g.is_array() || g.size() != 8) fail(path, "expected [codepoint, x, y, w, h, offsetX, offsetY, advance]");
            if (!g[0].is_number_integer() || !validCodepoint(g[0].get<std::int64_t>())) {
                fail(path, "the first number must be a Unicode codepoint");
            }
            Glyph glyph;
            glyph.codepoint = static_cast<char32_t>(g[0].get<std::int64_t>());
            const int x = readInt(g[1], path, 0, 16384);
            const int y = readInt(g[2], path, 0, 16384);
            const int w = readInt(g[3], path, 0, kMaxFontPixels);
            const int h = readInt(g[4], path, 0, kMaxFontPixels);
            glyph.source = {static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h)};
            glyph.offset = {static_cast<float>(readInt(g[5], path, -kMaxFontPixels, kMaxFontPixels)),
                            static_cast<float>(readInt(g[6], path, -kMaxFontPixels, kMaxFontPixels))};
            glyph.advance = static_cast<float>(readInt(g[7], path, 0, kMaxFontPixels));
            font.glyphs.push_back(glyph);
        }
        std::sort(font.glyphs.begin(), font.glyphs.end(),
                  [](const Glyph& a, const Glyph& b) { return a.codepoint < b.codepoint; });
        for (std::size_t i = 1; i < font.glyphs.size(); ++i) {
            if (font.glyphs[i].codepoint == font.glyphs[i - 1].codepoint) {
                fail("glyphs", std::format("U+{:04X} is listed twice", static_cast<std::uint32_t>(font.glyphs[i].codepoint)));
            }
        }
        r.finish();
        return font;
    } catch (const detail::FormatError& e) {
        return makeError(ErrorCode::InvalidData, "{}: {}", sourceName, e.what());
    }
}

std::string serializeFont(const FontData& font) {
    Json root;
    root["format"] = "px.font";
    root["formatVersion"] = kFontFormatVersion;
    root["texture"] = detail::toJson(font.texture);
    root["size"] = font.size;
    root["lineHeight"] = font.lineHeight;
    root["ascent"] = font.ascent;
    Json glyphs = Json::array();
    for (const Glyph& g : font.glyphs) {
        glyphs.push_back(Json::array({static_cast<std::uint32_t>(g.codepoint), static_cast<int>(g.source.x),
                                      static_cast<int>(g.source.y), static_cast<int>(g.source.w),
                                      static_cast<int>(g.source.h), static_cast<int>(std::lround(g.offset.x)),
                                      static_cast<int>(std::lround(g.offset.y)), static_cast<int>(std::lround(g.advance))}));
    }
    root["glyphs"] = std::move(glyphs);
    return detail::dumpPretty(root);
}

Result<FontData> loadFont(const IFileSystem& fs, const assets::AssetRegistry& assets, assets::AssetId id) {
    const auto* entry = assets.find(id);
    if (entry == nullptr) return makeError(ErrorCode::NotFound, "Font {} is not in the project", id.toString());
    if (entry->type != assets::AssetType::Font) {
        return makeError(ErrorCode::InvalidArgument, "Asset '{}' is not a font", entry->name);
    }
    auto text = fs.readText(entry->path);
    if (!text) return text.error();
    return parseFont(text.value(), fs.describe(entry->path), &assets);
}

Status checkFontTexture(const FontData& font, Vec2i size, std::string_view sourceName) {
    for (const Glyph& g : font.glyphs) {
        if (g.source.x + g.source.w > static_cast<float>(size.x) || g.source.y + g.source.h > static_cast<float>(size.y)) {
            return makeError(ErrorCode::InvalidData, "{}: the glyph of U+{:04X} is outside its {}x{} image", sourceName,
                             static_cast<std::uint32_t>(g.codepoint), size.x, size.y);
        }
    }
    return {};
}

} // namespace px::project
