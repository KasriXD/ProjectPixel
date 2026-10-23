#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/core/Guid.hpp"
#include "px/core/Math.hpp"
#include "px/core/Result.hpp"
#include "px/project/Project.hpp"

#include <string>
#include <string_view>
#include <vector>

// The starter library offered by Studio's Toolbox ("library.json", format
// "px.library"): images, tilesets and behaviours that can be copied into a
// project, and ready-made objects ("px.entities" documents, the same format
// as Studio's clipboard). Format reference: docs/library-format.md.
namespace px::project {

inline constexpr int kLibraryFormatVersion = 1;
inline constexpr std::string_view kLibraryFileName = "library.json";
inline constexpr std::size_t kMaxLibraryItems = 5000;

struct LibraryAsset {
    assets::AssetEntry entry; // id, type, name, path relative to the library folder, filter
    std::string category;     // grouping in the Toolbox ("Items", "Tiles", ...)
    std::string description;
    std::vector<GameType> gameTypes; // the kinds of game it fits; empty = all
    Vec2 tileSize{};                 // tilesets only
    int tilesetColumns = 0;          // > 0 for tilesets (dropping one creates a tilemap)

    [[nodiscard]] bool isTileset() const { return tilesetColumns > 0; }
    [[nodiscard]] bool fits(GameType type) const;
};

struct LibraryObject {
    Guid id;
    std::string name;
    std::string category;
    std::string description;
    std::vector<GameType> gameTypes; // empty = all
    std::string path;                // the "px.entities" document, relative to the library folder
    assets::AssetId iconTexture;     // a library texture shown in the Toolbox
    Rect iconSource{};               // part of it (empty = whole image)

    [[nodiscard]] bool fits(GameType type) const;
};

struct LibraryManifest {
    std::string name;
    std::vector<LibraryAsset> assets;
    std::vector<LibraryObject> objects;

    [[nodiscard]] const LibraryAsset* findAsset(const assets::AssetId& id) const;
    // The library's assets as a registry (paths relative to the library folder).
    [[nodiscard]] assets::AssetRegistry registry() const;
};

[[nodiscard]] Result<LibraryManifest> parseLibraryManifest(std::string_view json, std::string_view sourceName);

} // namespace px::project
