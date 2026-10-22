#include "px/assets/AssetRegistry.hpp"

#include "px/core/FileSystem.hpp"

namespace px::assets {

std::string_view assetTypeName(AssetType type) noexcept {
    switch (type) {
        case AssetType::Texture: return "texture";
        case AssetType::Scene: return "scene";
    }
    return "unknown";
}

std::optional<AssetType> assetTypeFromName(std::string_view name) noexcept {
    if (name == "texture") return AssetType::Texture;
    if (name == "scene") return AssetType::Scene;
    return std::nullopt;
}

std::string_view textureFilterName(TextureFilter filter) noexcept {
    return filter == TextureFilter::Nearest ? "nearest" : "linear";
}

std::optional<TextureFilter> textureFilterFromName(std::string_view name) noexcept {
    if (name == "nearest") return TextureFilter::Nearest;
    if (name == "linear") return TextureFilter::Linear;
    return std::nullopt;
}

Status AssetRegistry::add(AssetEntry entry) {
    if (entry.id.isNil()) {
        return makeError(ErrorCode::InvalidData, "Asset '{}' has a nil id", entry.name);
    }
    if (m_index.contains(entry.id)) {
        return makeError(ErrorCode::InvalidData, "Duplicate asset id {} ('{}' and '{}')", entry.id.toString(),
                         m_entries[m_index[entry.id]].name, entry.name);
    }
    auto path = normalizeRelativePath(entry.path);
    if (!path) {
        return makeError(path.error().code, "Asset '{}': {}", entry.name, path.error().message);
    }
    entry.path = std::move(path.value());
    m_index.emplace(entry.id, m_entries.size());
    m_entries.push_back(std::move(entry));
    return {};
}

const AssetEntry* AssetRegistry::find(AssetId id) const {
    const auto it = m_index.find(id);
    return it == m_index.end() ? nullptr : &m_entries[it->second];
}

const AssetEntry* AssetRegistry::findByName(std::string_view name) const {
    for (const auto& entry : m_entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

} // namespace px::assets
