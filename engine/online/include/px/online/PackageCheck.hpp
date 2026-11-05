#pragma once

#include "px/core/Result.hpp"
#include "px/project/Package.hpp"
#include "px/project/Project.hpp"

#include <string>
#include <vector>

// Checking an uploaded game before the platform accepts it. The platform
// runs this in a separate, resource-limited process (px-headless
// --check-package), never in the service itself: decoding images and
// running scripts of untrusted content is exactly what could go wrong.
namespace px::online {

struct PackageCheckOptions {
    int ticks = 120;            // simulate this many ticks of the start scene (0: none)
    bool reencodeImages = true; // replace every image with a PNG the engine wrote
};

struct PackageReport {
    std::string name;       // the project's name
    std::string gameType;   // "platformer", "topdown"
    std::size_t files = 0;
    std::size_t bytes = 0;  // after re-encoding
    std::size_t images = 0; // re-encoded images
    std::size_t scenes = 0;
    std::size_t scripts = 0; // scripts and visual scripts
    std::size_t entities = 0; // in the start scene
    std::size_t spawnPoints = 0; // Player entities in the start scene (multiplayer capacity hint)
    int scriptErrors = 0;     // runtime errors while simulating (reported, not fatal)
    std::string firstScriptError;
    std::vector<std::string> warnings;
    std::vector<project::LeaderboardDef> leaderboards; // declared in project.json
};

// Validates `package` completely (manifest, every scene, prefab, animation,
// tileset, script and visual script, every image), re-encodes its images in
// place and simulates the start scene. Fails if anything does not load.
[[nodiscard]] Result<PackageReport> checkPackage(project::Package& package, const PackageCheckOptions& options = {});

[[nodiscard]] std::string reportToJson(const PackageReport& report);
// Leaderboard declarations as the platform stores them per version.
[[nodiscard]] std::string leaderboardsToJson(const std::vector<project::LeaderboardDef>& boards);
[[nodiscard]] Result<std::vector<project::LeaderboardDef>> leaderboardsFromJson(std::string_view json);
[[nodiscard]] Result<PackageReport> reportFromJson(std::string_view json);

} // namespace px::online
