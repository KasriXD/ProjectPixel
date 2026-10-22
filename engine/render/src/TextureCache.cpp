#include "px/render/TextureCache.hpp"

#include "px/core/Log.hpp"

namespace px::render {

TextureCache::TextureCache(IRenderer2D& renderer, const IFileSystem& files, const assets::AssetRegistry& assets)
    : m_renderer(renderer), m_files(files), m_assets(assets) {}

TextureCache::~TextureCache() { clear(); }

void TextureCache::clear() {
    for (const auto& [id, handle] : m_textures) m_renderer.destroyTexture(handle);
    m_textures.clear();
    m_failed.clear();
    if (m_missing != kNoTexture) {
        m_renderer.destroyTexture(m_missing);
        m_missing = kNoTexture;
    }
}

TextureHandle TextureCache::missingTexture() {
    if (m_missing == kNoTexture) {
        assets::Image image;
        image.width = 8;
        image.height = 8;
        image.pixels.resize(8 * 8 * 4);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const bool magenta = ((x / 4) + (y / 4)) % 2 == 0;
                auto* p = &image.pixels[static_cast<std::size_t>((y * 8 + x) * 4)];
                p[0] = magenta ? 255 : 0;
                p[1] = 0;
                p[2] = magenta ? 255 : 0;
                p[3] = 255;
            }
        }
        m_missing = m_renderer.createTexture(image, assets::TextureFilter::Nearest);
    }
    return m_missing;
}

TextureHandle TextureCache::get(assets::AssetId id) {
    if (const auto it = m_textures.find(id); it != m_textures.end()) return it->second;
    if (m_failed.contains(id)) return missingTexture();

    const auto fail = [&](const std::string& message) {
        log::error("Texture {}: {}", id.toString(), message);
        m_failed.insert(id);
        return missingTexture();
    };

    const auto* entry = m_assets.find(id);
    if (entry == nullptr) return fail("not found in the project's assets");
    if (entry->type != assets::AssetType::Texture) return fail("asset '" + entry->name + "' is not a texture");

    auto bytes = m_files.readBinary(entry->path);
    if (!bytes) return fail(bytes.error().message);
    auto image = assets::decodeImage(bytes.value(), m_files.describe(entry->path));
    if (!image) return fail(image.error().message);

    const TextureHandle handle = m_renderer.createTexture(image.value(), entry->filter);
    if (handle == kNoTexture) return fail("the renderer could not create the texture");
    log::debug("Loaded texture '{}' ({}x{})", entry->name, image->width, image->height);
    m_textures.emplace(id, handle);
    return handle;
}

} // namespace px::render
