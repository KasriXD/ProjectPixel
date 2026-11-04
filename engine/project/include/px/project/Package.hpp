#pragma once

#include "px/core/FileSystem.hpp"
#include "px/core/Result.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

// Game packages ("*.pxpkg"): a published game as one file, the unit the
// platform stores, validates and sends to players and game servers. A
// package holds project.json and every file its assets refer to; nothing
// else from the project folder (Studio settings, tests, notes) is included.
// Format: docs/platform.md#game-packages.
namespace px::project {

inline constexpr std::uint16_t kPackageFormatVersion = 1;
inline constexpr std::size_t kMaxPackageFiles = 10001;          // the manifest + kMaxAssets
inline constexpr std::size_t kMaxPackageBytes = 256u << 20;     // 256 MiB
inline constexpr std::size_t kMaxPackagePathLength = 512;

struct PackageFile {
    std::string path; // normalized relative path ("scenes/level1.scene.json")
    std::vector<std::uint8_t> data;
};

struct Package {
    std::vector<PackageFile> files; // sorted by path, no duplicates, includes project.json

    [[nodiscard]] const PackageFile* find(std::string_view path) const;
    [[nodiscard]] std::size_t totalBytes() const;
    // The files as an in-memory file tree that games load from.
    [[nodiscard]] std::unique_ptr<MemoryFileSystem> toFileSystem() const;
};

// Collects project.json and every asset of the project in `files`. Missing
// asset files are an error: a package must be complete.
[[nodiscard]] Result<Package> collectPackage(const IFileSystem& files);
// The same for a project folder on disk.
[[nodiscard]] Result<Package> collectPackage(const std::filesystem::path& projectDir);

// Binary encoding. Deterministic: the same files give the same bytes.
[[nodiscard]] std::vector<std::uint8_t> encodePackage(const Package& package);
// Strict decoding of untrusted bytes: limits, path rules, no duplicates,
// project.json present.
[[nodiscard]] Result<Package> decodePackage(std::span<const std::uint8_t> bytes);

[[nodiscard]] Result<Package> readPackageFile(const std::filesystem::path& file);
[[nodiscard]] Status writePackageFile(const std::filesystem::path& file, const Package& package);

} // namespace px::project
