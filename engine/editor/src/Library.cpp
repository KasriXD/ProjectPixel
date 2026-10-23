#include "px/editor/Library.hpp"

#include "px/assets/Image.hpp"
#include "px/core/FileSystem.hpp"
#include "px/editor/EditorProject.hpp"
#include "px/editor/SceneDocument.hpp"
#include "px/project/SceneFormat.hpp"
#include "px/scene/ComponentInfo.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <span>
#include <sstream>

namespace px::editor {

using namespace px::scene;

namespace {

Result<std::string> readText(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return makeError(ErrorCode::NotFound, "{} is missing", path.generic_string());
    }
    if (std::filesystem::file_size(path, ec) > IFileSystem::kMaxFileSize) {
        return makeError(ErrorCode::LimitExceeded, "{} is too large", path.generic_string());
    }
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    if (!in && !in.eof()) return makeError(ErrorCode::IoError, "Cannot read {}", path.generic_string());
    return text.str();
}

// Puts `entity` (a tilemap) so that the map is centred on `centre`, with its
// corner on a multiple of its tile size.
void centreTilemap(Scene& scene, Entity entity, Vec2 centre) {
    const auto& map = scene.get<Tilemap>(entity);
    const Vec2 size{static_cast<float>(map.width) * map.tileSize.x, static_cast<float>(map.height) * map.tileSize.y};
    Vec2 corner = centre - size * 0.5f;
    if (map.tileSize.x > 0.0f) corner.x = std::round(corner.x / map.tileSize.x) * map.tileSize.x;
    if (map.tileSize.y > 0.0f) corner.y = std::round(corner.y / map.tileSize.y) * map.tileSize.y;
    scene.setWorldPosition(entity, corner);
}

} // namespace

Result<Library> Library::load(const std::filesystem::path& folder) {
    Library library;
    library.m_root = folder;
    const std::filesystem::path indexPath = folder / std::string(project::kLibraryFileName);
    auto text = readText(indexPath);
    if (!text) return text.error();
    auto manifest = project::parseLibraryManifest(text.value(), indexPath.generic_string());
    if (!manifest) return manifest.error();
    library.m_manifest = std::move(manifest.value());

    for (const project::LibraryAsset& asset : library.m_manifest.assets) {
        const std::filesystem::path file = library.assetFile(asset.entry.id);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            return makeError(ErrorCode::NotFound, "Library asset '{}': {} is missing", asset.entry.name,
                             file.generic_string());
        }
        if (asset.entry.type == assets::AssetType::Texture) {
            auto bytes = readText(file);
            if (!bytes) return bytes.error();
            const std::span<const std::uint8_t> data(reinterpret_cast<const std::uint8_t*>(bytes->data()),
                                                     bytes->size());
            auto image = assets::decodeImage(data, asset.entry.name);
            if (!image) {
                return makeError(ErrorCode::InvalidData, "Library image '{}': {}", asset.entry.name,
                                 image.error().message);
            }
            library.m_imageSizes[asset.entry.id] = {image->width, image->height};
        }
    }

    const assets::AssetRegistry registry = library.m_manifest.registry();
    for (const project::LibraryObject& object : library.m_manifest.objects) {
        auto objectText = readText(folder / std::filesystem::path(object.path));
        if (!objectText) return makeError(objectText.error().code, "Library object '{}': {}", object.name,
                                          objectText.error().message);
        Scene scratch;
        auto pasted = project::pasteEntities(scratch, objectText.value(), &registry);
        if (!pasted) {
            return makeError(ErrorCode::InvalidData, "Library object '{}' ({}): {}", object.name, object.path,
                             pasted.error().message);
        }
        if (pasted->empty()) {
            return makeError(ErrorCode::InvalidData, "Library object '{}' ({}) has no entities", object.name,
                             object.path);
        }
        ObjectData data;
        data.text = std::move(objectText.value());
        for (const Entity e : pasted.value()) {
            for (const assets::AssetId& id : assetReferences(scratch, e)) {
                if (std::find(data.assets.begin(), data.assets.end(), id) == data.assets.end()) data.assets.push_back(id);
            }
        }
        library.m_objects.push_back(std::move(data));
    }
    return library;
}

std::filesystem::path Library::assetFile(const assets::AssetId& id) const {
    const auto* asset = m_manifest.findAsset(id);
    return asset == nullptr ? std::filesystem::path() : m_root / std::filesystem::path(asset->entry.path);
}

Vec2i Library::imageSize(const assets::AssetId& id) const {
    const auto it = m_imageSizes.find(id);
    return it == m_imageSizes.end() ? Vec2i{} : it->second;
}

int Library::findObject(const Guid& id) const {
    const auto& objects = m_manifest.objects;
    const auto it = std::find_if(objects.begin(), objects.end(), [&](const auto& o) { return o.id == id; });
    return it == objects.end() ? -1 : static_cast<int>(it - objects.begin());
}

Result<std::vector<Entity>> insertLibraryObject(EditorProject& project, SceneDocument& document, const Library& library,
                                               std::size_t index, Vec2 position, Entity parent) {
    if (index >= library.manifest().objects.size()) {
        return makeError(ErrorCode::NotFound, "The library has no object {}", index);
    }
    const project::LibraryObject& object = library.manifest().objects[index];
    for (const assets::AssetId& id : library.objectAssets(index)) {
        const auto* asset = library.manifest().findAsset(id);
        if (asset == nullptr) return makeError(ErrorCode::NotFound, "'{}' needs a missing library asset", object.name);
        if (auto added = project.addLibraryAsset(*asset, library.assetFile(id)); !added) return added.error();
    }

    document.beginEdit(std::format("Insert {}", object.name));
    auto pasted = document.paste(library.objectText(index), &project.assets(), parent);
    if (!pasted) {
        document.cancelEdit();
        return pasted.error();
    }
    Scene& scene = document.sceneForEdit();
    std::vector<Entity> roots;
    for (const Entity e : pasted.value()) {
        const Entity p = scene.parent(e);
        if (std::find(pasted->begin(), pasted->end(), p) == pasted->end()) roots.push_back(e);
    }
    const auto anchor = std::find_if(roots.begin(), roots.end(), [&](Entity e) { return scene.tryGet<Transform>(e); });
    if (anchor != roots.end()) {
        const Vec2 before = scene.worldPosition(*anchor);
        if (scene.tryGet<Tilemap>(*anchor) != nullptr) {
            centreTilemap(scene, *anchor, position);
        } else {
            scene.setWorldPosition(*anchor, position);
        }
        const Vec2 delta = scene.worldPosition(*anchor) - before;
        for (const Entity e : roots) {
            if (e != *anchor && scene.tryGet<Transform>(e) != nullptr) {
                scene.setWorldPosition(e, scene.worldPosition(e) + delta);
            }
        }
    }
    for (const Entity e : pasted.value()) document.followIfCameraIdle(e);
    document.markChanged();
    document.endEdit();
    return pasted;
}

Result<Entity> insertLibraryImage(EditorProject& project, SceneDocument& document, const Library& library,
                                  const assets::AssetId& image, Vec2 position) {
    const auto* asset = library.manifest().findAsset(image);
    if (asset == nullptr || asset->entry.type != assets::AssetType::Texture) {
        return makeError(ErrorCode::NotFound, "Not a library image");
    }
    auto added = project.addLibraryAsset(*asset, library.assetFile(image));
    if (!added) return added.error();

    document.beginEdit(std::format("Insert {}", asset->entry.name));
    Entity e;
    if (asset->isTileset()) {
        e = document.createEntity(EntityPreset::Tilemap, asset->entry.name, kNullEntity, position, added.value());
        Scene& scene = document.sceneForEdit();
        auto& map = scene.get<Tilemap>(e);
        map.tileSize = asset->tileSize;
        map.tilesetColumns = asset->tilesetColumns;
        centreTilemap(scene, e, position);
    } else {
        e = document.createEntity(EntityPreset::Sprite, asset->entry.name, kNullEntity, position, added.value());
        const Vec2i size = library.imageSize(image);
        if (size.x > 0 && size.y > 0) {
            document.sceneForEdit().get<Sprite>(e).size = {static_cast<float>(size.x), static_cast<float>(size.y)};
        }
    }
    document.markChanged();
    document.endEdit();
    return e;
}

Status attachLibraryScript(EditorProject& project, SceneDocument& document, const Library& library,
                           const assets::AssetId& script, Entity entity) {
    const auto* asset = library.manifest().findAsset(script);
    if (asset == nullptr || asset->entry.type != assets::AssetType::Script) {
        return makeError(ErrorCode::NotFound, "Not a library behaviour");
    }
    auto added = project.addLibraryAsset(*asset, library.assetFile(script));
    if (!added) return added.error();
    return document.attachScript(entity, added.value());
}

} // namespace px::editor
