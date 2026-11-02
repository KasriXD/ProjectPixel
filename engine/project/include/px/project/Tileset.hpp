#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/core/FileSystem.hpp"
#include "px/core/Math.hpp"
#include "px/core/Result.hpp"
#include "px/project/Project.hpp"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Tilesets ("*.tileset.json"): what the tiles of a tileset image are. Every
// tilemap painted with that image uses them: which tiles are solid, one-way
// floors or slopes, and tags scripts can test (docs/tilesets.md).
namespace px::project {

inline constexpr int kTilesetFormatVersion = 1;
inline constexpr int kMaxTileIndex = 65535;
inline constexpr std::size_t kMaxTileTags = 8;
inline constexpr std::size_t kMaxTileTagLength = 32;

// How a tile blocks movement in a tilemap whose `solid` is on.
enum class TileCollision {
    Solid,        // blocks from every side (the default)
    None,         // decoration: does not block
    OneWay,       // a floor: jump up through it, walk through it sideways
    SlopeUpRight, // a 45-degree floor rising to the right ('/')
    SlopeUpLeft,  // a 45-degree floor rising to the left ('\')
};
[[nodiscard]] std::string_view tileCollisionName(TileCollision c) noexcept; // "solid", "none", "oneWay", ...
[[nodiscard]] std::optional<TileCollision> parseTileCollision(std::string_view name) noexcept;

struct TileInfo {
    TileCollision collision = TileCollision::Solid;
    std::vector<std::string> tags; // e.g. "water", "spikes"

    [[nodiscard]] bool isDefault() const { return collision == TileCollision::Solid && tags.empty(); }
    [[nodiscard]] bool hasTag(std::string_view tag) const;
};

struct Tileset {
    assets::AssetId texture;     // the image the tiles come from
    Vec2 tileSize{16.0f, 16.0f}; // tilemaps with another tile size ignore this tileset
    std::map<int, TileInfo> tiles; // tiles that are not plain solid tiles, by index

    [[nodiscard]] const TileInfo& info(int tile) const; // the default for unlisted tiles
    // Sets a tile's information (removing it from `tiles` when it is the default).
    void set(int tile, TileInfo info);
};

[[nodiscard]] Result<Tileset> parseTileset(std::string_view json, std::string_view sourceName,
                                           const assets::AssetRegistry* assets = nullptr);
[[nodiscard]] std::string serializeTileset(const Tileset& tileset);
[[nodiscard]] Result<Tileset> loadTileset(const IFileSystem& fs, const ProjectManifest& project, assets::AssetId id);

// The tilesets of a project. A tilemap uses the tileset of its image.
class TilesetLibrary {
public:
    struct Entry {
        assets::AssetId id;
        std::string name;
        Tileset tileset;
    };
    void add(Entry entry);
    [[nodiscard]] const Entry* find(assets::AssetId id) const;
    // The tileset made for `texture` with tiles of `tileSize`, or nullptr.
    [[nodiscard]] const Tileset* forTexture(assets::AssetId texture, Vec2 tileSize) const;
    [[nodiscard]] std::size_t size() const { return m_entries.size(); }

private:
    std::vector<Entry> m_entries;
};

[[nodiscard]] Result<std::shared_ptr<const TilesetLibrary>> loadTilesetLibrary(const IFileSystem& fs,
                                                                                const ProjectManifest& project);

} // namespace px::project
