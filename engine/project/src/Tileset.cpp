#include "px/project/Tileset.hpp"

#include "JsonUtil.hpp"

#include <algorithm>
#include <format>
#include <set>

namespace px::project {

using detail::fail;
using detail::Json;
using detail::ObjectReader;

std::string_view tileCollisionName(TileCollision c) noexcept {
    switch (c) {
        case TileCollision::Solid: return "solid";
        case TileCollision::None: return "none";
        case TileCollision::OneWay: return "oneWay";
        case TileCollision::SlopeUpRight: return "slopeUpRight";
        case TileCollision::SlopeUpLeft: return "slopeUpLeft";
    }
    return "solid";
}

std::optional<TileCollision> parseTileCollision(std::string_view name) noexcept {
    for (const TileCollision c : {TileCollision::Solid, TileCollision::None, TileCollision::OneWay,
                                  TileCollision::SlopeUpRight, TileCollision::SlopeUpLeft}) {
        if (tileCollisionName(c) == name) return c;
    }
    return std::nullopt;
}

bool TileInfo::hasTag(std::string_view tag) const { return std::find(tags.begin(), tags.end(), tag) != tags.end(); }

const TileInfo& Tileset::info(int tile) const {
    static const TileInfo kDefault;
    const auto it = tiles.find(tile);
    return it == tiles.end() ? kDefault : it->second;
}

void Tileset::set(int tile, TileInfo value) {
    if (value.isDefault()) {
        tiles.erase(tile);
    } else {
        tiles[tile] = std::move(value);
    }
}

Result<Tileset> parseTileset(std::string_view json, std::string_view sourceName, const assets::AssetRegistry* assets) {
    try {
        Json root = detail::parseJsonText(json);
        std::vector<std::string> warnings;
        detail::checkAndMigrate(root, "px.tileset", kTilesetFormatVersion, {}, "tileset", warnings);
        ObjectReader r(root, "");
        (void)r.find("format");
        (void)r.find("formatVersion");
        Tileset tileset;
        r.required("texture", tileset.texture);
        if (tileset.texture.isNil()) fail("texture", "a texture asset id is required");
        if (assets != nullptr) {
            const auto* entry = assets->find(tileset.texture);
            if (entry == nullptr) {
                fail("texture", std::format("refers to asset {}, which is not in the project", tileset.texture.toString()));
            }
            if (entry->type != assets::AssetType::Texture) {
                fail("texture", std::format("asset '{}' is a {}, not a texture", entry->name,
                                            assets::assetTypeName(entry->type)));
            }
        }
        r.optional("tileSize", tileset.tileSize);
        if (!(tileset.tileSize.x >= 1.0f) || !(tileset.tileSize.y >= 1.0f) || tileset.tileSize.x > 4096.0f ||
            tileset.tileSize.y > 4096.0f) {
            fail("tileSize", "must be between [1, 1] and [4096, 4096]");
        }
        if (const Json* tiles = r.find("tiles")) {
            if (!tiles->is_array()) fail("tiles", "expected an array of tiles");
            if (tiles->size() > 4096) fail("tiles", "too many tiles; the limit is 4096");
            std::set<int> seen;
            for (std::size_t i = 0; i < tiles->size(); ++i) {
                const std::string path = detail::indexPath("tiles", i);
                ObjectReader t((*tiles)[i], path);
                int index = 0;
                t.required("tile", index);
                if (index < 0 || index > kMaxTileIndex) fail(detail::childPath(path, "tile"), "must be 0 to 65535");
                if (!seen.insert(index).second) {
                    fail(detail::childPath(path, "tile"), std::format("tile {} is listed twice", index));
                }
                TileInfo info;
                std::string collision = "solid";
                t.optional("collision", collision);
                const auto parsed = parseTileCollision(collision);
                if (!parsed) {
                    fail(detail::childPath(path, "collision"),
                         std::format("unknown collision '{}'; use \"solid\", \"none\", \"oneWay\", \"slopeUpRight\" "
                                     "or \"slopeUpLeft\"",
                                     collision));
                }
                info.collision = *parsed;
                if (const Json* tags = t.find("tags")) {
                    const std::string tagsPath = detail::childPath(path, "tags");
                    if (!tags->is_array()) fail(tagsPath, "expected an array of strings");
                    if (tags->size() > kMaxTileTags) fail(tagsPath, std::format("at most {} tags", kMaxTileTags));
                    for (std::size_t k = 0; k < tags->size(); ++k) {
                        std::string tag;
                        detail::readValue((*tags)[k], detail::indexPath(tagsPath, k), tag);
                        if (tag.empty() || tag.size() > kMaxTileTagLength) {
                            fail(detail::indexPath(tagsPath, k), "tags must be 1 to 32 characters");
                        }
                        if (!info.hasTag(tag)) info.tags.push_back(std::move(tag));
                    }
                }
                t.finish();
                tileset.set(index, std::move(info));
            }
        }
        r.finish();
        return tileset;
    } catch (const detail::FormatError& e) {
        return makeError(ErrorCode::InvalidData, "{}: {}", sourceName, e.what());
    }
}

std::string serializeTileset(const Tileset& tileset) {
    Json root;
    root["format"] = "px.tileset";
    root["formatVersion"] = kTilesetFormatVersion;
    root["texture"] = detail::toJson(tileset.texture);
    root["tileSize"] = detail::toJson(tileset.tileSize);
    Json tiles = Json::array();
    for (const auto& [index, info] : tileset.tiles) {
        if (info.isDefault()) continue;
        Json t;
        t["tile"] = index;
        t["collision"] = std::string(tileCollisionName(info.collision));
        if (!info.tags.empty()) {
            Json tags = Json::array();
            for (const auto& tag : info.tags) tags.push_back(tag);
            t["tags"] = std::move(tags);
        }
        tiles.push_back(std::move(t));
    }
    root["tiles"] = std::move(tiles);
    return detail::dumpPretty(root);
}

Result<Tileset> loadTileset(const IFileSystem& fs, const ProjectManifest& project, assets::AssetId id) {
    const auto* entry = project.assets.find(id);
    if (entry == nullptr) return makeError(ErrorCode::NotFound, "Tileset {} is not in the project", id.toString());
    if (entry->type != assets::AssetType::Tileset) {
        return makeError(ErrorCode::InvalidArgument, "Asset '{}' is not a tileset", entry->name);
    }
    auto text = fs.readText(entry->path);
    if (!text) return text.error();
    return parseTileset(text.value(), fs.describe(entry->path), &project.assets);
}

void TilesetLibrary::add(Entry entry) { m_entries.push_back(std::move(entry)); }

const TilesetLibrary::Entry* TilesetLibrary::find(assets::AssetId id) const {
    for (const Entry& e : m_entries) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

const Tileset* TilesetLibrary::forTexture(assets::AssetId texture, Vec2 tileSize) const {
    for (const Entry& e : m_entries) {
        if (e.tileset.texture == texture && e.tileset.tileSize == tileSize) return &e.tileset;
    }
    return nullptr;
}

Result<std::shared_ptr<const TilesetLibrary>> loadTilesetLibrary(const IFileSystem& fs, const ProjectManifest& project) {
    auto library = std::make_shared<TilesetLibrary>();
    for (const auto& entry : project.assets.entries()) {
        if (entry.type != assets::AssetType::Tileset) continue;
        auto tileset = loadTileset(fs, project, entry.id);
        if (!tileset) return tileset.error();
        library->add({entry.id, entry.name, std::move(tileset.value())});
    }
    return std::shared_ptr<const TilesetLibrary>(std::move(library));
}

} // namespace px::project
