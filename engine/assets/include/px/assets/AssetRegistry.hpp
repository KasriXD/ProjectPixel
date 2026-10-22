#pragma once

#include "px/assets/AssetId.hpp"
#include "px/core/Result.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace px::assets {

enum class AssetType { Texture, Scene, Script };

[[nodiscard]] std::string_view assetTypeName(AssetType type) noexcept;
[[nodiscard]] std::optional<AssetType> assetTypeFromName(std::string_view name) noexcept;

// How a texture is sampled when scaled. Nearest keeps pixel art crisp.
enum class TextureFilter { Nearest, Linear };

[[nodiscard]] std::string_view textureFilterName(TextureFilter filter) noexcept;
[[nodiscard]] std::optional<TextureFilter> textureFilterFromName(std::string_view name) noexcept;

struct AssetEntry {
    AssetId id;
    AssetType type = AssetType::Texture;
    std::string name; // human-readable label shown in tools
    std::string path; // normalized path relative to the project root
    TextureFilter filter = TextureFilter::Nearest; // textures only
};

// The set of assets belonging to a project, keyed by stable id.
class AssetRegistry {
public:
    // Fails on nil or duplicate ids and on invalid paths.
    [[nodiscard]] Status add(AssetEntry entry);

    [[nodiscard]] const AssetEntry* find(AssetId id) const;
    [[nodiscard]] const AssetEntry* findByName(std::string_view name) const;
    [[nodiscard]] const std::vector<AssetEntry>& entries() const { return m_entries; }
    [[nodiscard]] std::size_t size() const { return m_entries.size(); }

private:
    std::vector<AssetEntry> m_entries;
    std::unordered_map<AssetId, std::size_t> m_index;
};

} // namespace px::assets
