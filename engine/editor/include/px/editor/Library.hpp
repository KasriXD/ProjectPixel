#pragma once

#include "px/assets/AssetId.hpp"
#include "px/core/Math.hpp"
#include "px/core/Result.hpp"
#include "px/project/Library.hpp"
#include "px/scene/Scene.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Studio's starter library (library/library.json): images, tilesets,
// behaviours and ready-made objects that can be put into any project.
// Inserting something copies the files it needs into the project with the
// library's asset ids, so inserting it again reuses them.
namespace px::editor {

class EditorProject;
class SceneDocument;

class Library {
public:
    // Reads library.json and checks it: every file it names must exist,
    // images must decode, and every object must be a valid entities
    // document that uses only the library's own assets.
    [[nodiscard]] static Result<Library> load(const std::filesystem::path& folder);

    [[nodiscard]] const project::LibraryManifest& manifest() const { return m_manifest; }
    [[nodiscard]] const std::filesystem::path& root() const { return m_root; }
    [[nodiscard]] std::filesystem::path assetFile(const assets::AssetId& id) const;
    // Pixel size of a library image (0, 0 for scripts or unknown ids).
    [[nodiscard]] Vec2i imageSize(const assets::AssetId& id) const;

    [[nodiscard]] const std::string& objectText(std::size_t index) const { return m_objects[index].text; }
    // Library assets the object's entities refer to (textures and scripts).
    [[nodiscard]] const std::vector<assets::AssetId>& objectAssets(std::size_t index) const {
        return m_objects[index].assets;
    }
    // Index of the object with this id, or -1.
    [[nodiscard]] int findObject(const Guid& id) const;

private:
    struct ObjectData {
        std::string text;
        std::vector<assets::AssetId> assets;
    };
    std::filesystem::path m_root;
    project::LibraryManifest m_manifest;
    std::vector<ObjectData> m_objects;
    std::map<assets::AssetId, Vec2i> m_imageSizes;
};

// Inserts a ready-made object into the open scene as one undo step: copies
// the assets it needs into the project, then pastes its entities under
// `parent` (selected afterwards). The first top-level entity is placed at
// `position`; a tilemap is instead centred on it, with its corner on a tile
// boundary. Others keep their offsets from the first.
[[nodiscard]] Result<std::vector<scene::Entity>> insertLibraryObject(EditorProject& project, SceneDocument& document,
                                                                    const Library& library, std::size_t object,
                                                                    Vec2 position,
                                                                    scene::Entity parent = scene::kNullEntity);

// Puts a library image into the scene as one undo step: a tileset becomes an
// empty tilemap (20 x 10 tiles) centred on `position`, any other image a
// sprite of its size at `position`. The image is copied into the project.
[[nodiscard]] Result<scene::Entity> insertLibraryImage(EditorProject& project, SceneDocument& document,
                                                       const Library& library, const assets::AssetId& image,
                                                       Vec2 position);

// Copies a library behaviour into the project (if needed) and attaches it.
[[nodiscard]] Status attachLibraryScript(EditorProject& project, SceneDocument& document, const Library& library,
                                         const assets::AssetId& script, scene::Entity entity);

} // namespace px::editor
