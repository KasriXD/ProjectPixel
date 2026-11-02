#include "px/runtime/GameLoader.hpp"

#include "px/project/SceneFormat.hpp"

namespace px::runtime {

Result<LoadedGame> loadGame(const IFileSystem& files) {
    LoadedGame game;
    auto manifest = project::loadProjectManifest(files);
    if (!manifest) return manifest.error();
    game.manifest = std::move(manifest.value());

    const auto* entry = game.manifest.assets.find(game.manifest.startScene);
    if (entry == nullptr) return makeError(ErrorCode::NotFound, "The start scene is not listed in the project");
    auto text = files.readText(entry->path);
    if (!text) return text.error();
    auto scene = project::parseScene(text.value(), files.describe(entry->path), &game.manifest.assets, &game.warnings);
    if (!scene) return scene.error();
    game.startScene = std::move(scene.value());

    auto prefabs = project::loadPrefabLibrary(files, game.manifest, &game.warnings);
    if (!prefabs) return prefabs.error();
    game.prefabs = std::move(prefabs.value());

    auto scripts = scripting::loadScriptLibrary(files, game.manifest.assets);
    if (!scripts) return scripts.error();
    game.scripts = std::move(scripts.value());
    return game;
}

Result<std::unique_ptr<GameSession>> startSession(const LoadedGame& game, const SessionOptions& options) {
    if (options.prefabs != nullptr || game.prefabs == nullptr) {
        return GameSession::create(game.manifest, game.startScene, game.scripts, options);
    }
    SessionOptions withPrefabs = options;
    withPrefabs.prefabs = game.prefabs;
    return GameSession::create(game.manifest, game.startScene, game.scripts, withPrefabs);
}

} // namespace px::runtime
