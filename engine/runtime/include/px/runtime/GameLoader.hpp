#pragma once

#include "px/core/FileSystem.hpp"
#include "px/core/Result.hpp"
#include "px/project/Project.hpp"
#include "px/runtime/GameSession.hpp"
#include "px/scene/Scene.hpp"
#include "px/scripting/ScriptLibrary.hpp"

#include <memory>
#include <string>
#include <vector>

namespace px::runtime {

// Everything loaded from a project folder that is needed to start a game.
// Immutable after loading; sessions copy the start scene.
struct LoadedGame {
    project::ProjectManifest manifest;
    scene::Scene startScene;
    std::shared_ptr<const scripting::ScriptLibrary> scripts;
    std::vector<std::string> warnings; // e.g. format migration notes
};

// Loads the manifest, the start scene and all scripts (compiling them, so
// syntax errors are reported here).
[[nodiscard]] Result<LoadedGame> loadGame(const IFileSystem& files);

// Starts a session of the loaded game's start scene.
[[nodiscard]] Result<std::unique_ptr<GameSession>> startSession(const LoadedGame& game,
                                                                const SessionOptions& options = {});

} // namespace px::runtime
