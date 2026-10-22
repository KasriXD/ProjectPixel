#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/core/FileSystem.hpp"
#include "px/render/Renderer2D.hpp"

#include <unordered_map>
#include <unordered_set>

namespace px::render {

// Loads texture assets on first use and owns the GPU textures.
// A texture that cannot be loaded is replaced by a visible magenta/black
// checkerboard and the error is logged once, so a broken asset never stops
// a game from running.
class TextureCache {
public:
    TextureCache(IRenderer2D& renderer, const IFileSystem& files, const assets::AssetRegistry& assets);
    ~TextureCache();
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    [[nodiscard]] TextureHandle get(assets::AssetId id);
    [[nodiscard]] TextureHandle missingTexture();
    [[nodiscard]] bool isMissing(TextureHandle handle) const { return handle != kNoTexture && handle == m_missing; }

    void clear();

private:
    IRenderer2D& m_renderer;
    const IFileSystem& m_files;
    const assets::AssetRegistry& m_assets;
    std::unordered_map<assets::AssetId, TextureHandle> m_textures;
    std::unordered_set<assets::AssetId> m_failed;
    TextureHandle m_missing = kNoTexture;
};

} // namespace px::render
