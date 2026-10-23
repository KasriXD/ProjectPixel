#pragma once

#include "px/assets/AssetRegistry.hpp"
#include "px/core/FileSystem.hpp"
#include "px/core/Guid.hpp"
#include "px/core/Keys.hpp"
#include "px/core/Math.hpp"
#include "px/core/Result.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Project manifest: the root file of a game project ("project.json").
// Format reference: docs/project-format.md.
namespace px::project {

inline constexpr int kProjectFormatVersion = 2;
inline constexpr std::string_view kProjectFileName = "project.json";

// Hard limits that keep untrusted projects from exhausting resources.
inline constexpr std::size_t kMaxInputActions = 32;
inline constexpr std::size_t kMaxAssets = 10000;

enum class ScalingMode {
    Integer,   // largest whole-number scale that fits; best for pixel art
    Letterbox, // fractional scale preserving aspect ratio
};

struct DisplaySettings {
    Vec2i virtualResolution{320, 180}; // game's logical screen size in pixels
    ScalingMode scaling = ScalingMode::Integer;
    int windowScale = 3; // initial desktop window size = virtualResolution * windowScale
};

// Version of the gameplay scripting API this engine provides. Projects state
// the version they were written for; see docs/scripting.md#api-versioning.
inline constexpr int kScriptApiVersion = 1;

struct ScriptingSettings {
    int apiVersion = kScriptApiVersion;
};

// The kind of game, chosen when the project is created. It decides what
// Studio offers (the player preset, input actions, starter objects), not
// what the engine can run: any component works in any project.
enum class GameType {
    Platformer, // side view with gravity and jumping
    TopDown,    // seen from above, moving in all directions
};
[[nodiscard]] std::string_view gameTypeName(GameType type) noexcept; // "platformer", "topdown"
[[nodiscard]] std::optional<GameType> parseGameType(std::string_view name) noexcept;

struct ProjectSettings {
    GameType gameType = GameType::Platformer;
    int tickRate = 60; // fixed simulation steps per second
    DisplaySettings display;
    ScriptingSettings scripting;
};

struct InputAction {
    std::string name;      // e.g. "jump"; [a-z0-9_], used by gameplay code
    std::vector<Key> keys; // default keyboard bindings
};

struct InputConfig {
    std::vector<InputAction> actions; // order defines action ids

    [[nodiscard]] const InputAction* find(std::string_view name) const {
        for (const auto& a : actions) {
            if (a.name == name) return &a;
        }
        return nullptr;
    }
};

struct ProjectManifest {
    int formatVersion = kProjectFormatVersion;
    Guid id;
    std::string name;
    std::string engineVersion; // engine version that last wrote the file (informational)
    assets::AssetId startScene;
    ProjectSettings settings;
    InputConfig input;
    assets::AssetRegistry assets;
};

[[nodiscard]] Result<ProjectManifest> parseProjectManifest(std::string_view json, std::string_view sourceName);
[[nodiscard]] Result<ProjectManifest> loadProjectManifest(const IFileSystem& fs);
[[nodiscard]] std::string serializeProjectManifest(const ProjectManifest& manifest);

} // namespace px::project
