#include "px/project/Library.hpp"

#include "JsonUtil.hpp"

#include "px/core/FileSystem.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <set>

namespace px::project {

using detail::fail;
using detail::Json;
using detail::ObjectReader;

namespace {

constexpr std::array<detail::Migration, 0> kLibraryMigrations{};

std::vector<GameType> readGameTypes(ObjectReader& r) {
    std::vector<GameType> types;
    const Json* list = r.find("gameTypes");
    if (list == nullptr) return types;
    const std::string path = detail::childPath(r.path(), "gameTypes");
    if (!list->is_array()) fail(path, "expected an array of game types");
    for (std::size_t i = 0; i < list->size(); ++i) {
        std::string name;
        detail::readValue((*list)[i], detail::indexPath(path, i), name);
        const auto type = parseGameType(name);
        if (!type) {
            fail(detail::indexPath(path, i), std::format("unknown game type '{}'; use \"platformer\" or \"topdown\"",
                                                         name));
        }
        types.push_back(*type);
    }
    return types;
}

bool fitsType(const std::vector<GameType>& types, GameType type) {
    return types.empty() || std::find(types.begin(), types.end(), type) != types.end();
}

LibraryManifest readLibrary(Json& root) {
    std::vector<std::string> warnings;
    detail::checkAndMigrate(root, "px.library", kLibraryFormatVersion, kLibraryMigrations, "library", warnings);
    LibraryManifest m;
    ObjectReader r(root, "");
    (void)r.find("format");
    int version = 0;
    r.required("formatVersion", version);
    r.optional("name", m.name);

    std::set<Guid> ids;
    assets::AssetRegistry check; // validates ids and paths the same way as projects
    if (const Json* list = r.find("assets")) {
        if (!list->is_array()) fail("assets", "expected an array");
        if (list->size() > kMaxLibraryItems) fail("assets", "too many assets");
        for (std::size_t i = 0; i < list->size(); ++i) {
            const std::string itemPath = detail::indexPath("assets", i);
            ObjectReader a((*list)[i], itemPath);
            LibraryAsset asset;
            a.required("id", asset.entry.id);
            std::string typeName;
            a.required("type", typeName);
            const auto type = assets::assetTypeFromName(typeName);
            if (!type || *type == assets::AssetType::Scene) {
                fail(detail::childPath(itemPath, "type"),
                     std::format("'{}' cannot be in a library; use \"texture\" or \"script\"", typeName));
            }
            asset.entry.type = *type;
            a.required("path", asset.entry.path);
            a.required("name", asset.entry.name);
            if (asset.entry.type == assets::AssetType::Texture) {
                std::string filter = "nearest";
                a.optional("filter", filter);
                const auto parsed = assets::textureFilterFromName(filter);
                if (!parsed) fail(detail::childPath(itemPath, "filter"), std::format("unknown filter '{}'", filter));
                asset.entry.filter = *parsed;
            }
            a.optional("category", asset.category);
            a.optional("description", asset.description);
            asset.gameTypes = readGameTypes(a);
            if (a.optional("tilesetColumns", asset.tilesetColumns)) {
                if (asset.entry.type != assets::AssetType::Texture || asset.tilesetColumns < 1) {
                    fail(detail::childPath(itemPath, "tilesetColumns"), "only textures can be tilesets (columns >= 1)");
                }
                asset.tileSize = {16, 16};
                a.optional("tileSize", asset.tileSize);
                if (asset.tileSize.x <= 0 || asset.tileSize.y <= 0) {
                    fail(detail::childPath(itemPath, "tileSize"), "must be positive");
                }
            }
            a.finish();
            if (auto added = check.add(asset.entry); !added) fail(itemPath, added.error().message);
            ids.insert(asset.entry.id.guid);
            asset.entry.path = check.find(asset.entry.id)->path; // normalized
            m.assets.push_back(std::move(asset));
        }
    }
    if (const Json* list = r.find("objects")) {
        if (!list->is_array()) fail("objects", "expected an array");
        if (list->size() > kMaxLibraryItems) fail("objects", "too many objects");
        for (std::size_t i = 0; i < list->size(); ++i) {
            const std::string itemPath = detail::indexPath("objects", i);
            ObjectReader o((*list)[i], itemPath);
            LibraryObject object;
            o.required("id", object.id);
            if (object.id.isNil() || !ids.insert(object.id).second) {
                fail(detail::childPath(itemPath, "id"), "missing or duplicate id");
            }
            o.required("name", object.name);
            if (object.name.empty()) fail(detail::childPath(itemPath, "name"), "must not be empty");
            o.optional("category", object.category);
            o.optional("description", object.description);
            object.gameTypes = readGameTypes(o);
            o.required("path", object.path);
            const auto normalized = normalizeRelativePath(object.path);
            if (!normalized) fail(detail::childPath(itemPath, "path"), normalized.error().message);
            object.path = normalized.value();
            if (const Json* icon = o.find("icon")) {
                ObjectReader ic(*icon, detail::childPath(itemPath, "icon"));
                ic.required("texture", object.iconTexture);
                ic.optional("source", object.iconSource);
                ic.finish();
                const auto* entry = check.find(object.iconTexture);
                if (entry == nullptr || entry->type != assets::AssetType::Texture) {
                    fail(detail::childPath(itemPath, "icon.texture"), "must be a texture of this library");
                }
            }
            o.finish();
            m.objects.push_back(std::move(object));
        }
    }
    r.finish();
    return m;
}

} // namespace

bool LibraryAsset::fits(GameType type) const { return fitsType(gameTypes, type); }
bool LibraryObject::fits(GameType type) const { return fitsType(gameTypes, type); }

const LibraryAsset* LibraryManifest::findAsset(const assets::AssetId& id) const {
    const auto it = std::find_if(assets.begin(), assets.end(), [&](const LibraryAsset& a) { return a.entry.id == id; });
    return it == assets.end() ? nullptr : &*it;
}

assets::AssetRegistry LibraryManifest::registry() const {
    assets::AssetRegistry registry;
    for (const LibraryAsset& a : assets) (void)registry.add(a.entry);
    return registry;
}

Result<LibraryManifest> parseLibraryManifest(std::string_view json, std::string_view sourceName) {
    try {
        Json root = detail::parseJsonText(json);
        return readLibrary(root);
    } catch (const detail::FormatError& e) {
        return makeError(ErrorCode::InvalidData, "{}: {}", sourceName, e.what());
    }
}

} // namespace px::project
