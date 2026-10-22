#pragma once

#include "px/core/Result.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace px {

// Validates and normalizes a path *inside* a project or package.
// Project content is untrusted, so paths must be relative, use '/' separators,
// and may not contain "..", drive letters, or URL schemes.
// "a/./b//c.png" -> "a/b/c.png".
[[nodiscard]] Result<std::string> normalizeRelativePath(std::string_view path);

// Read access to a tree of files addressed by normalized relative paths.
// Abstracts over disk directories today and app bundles (Android/iOS),
// browser storage and downloaded packages later.
class IFileSystem {
public:
    virtual ~IFileSystem() = default;

    [[nodiscard]] virtual Result<std::vector<std::uint8_t>> readBinary(std::string_view path) const = 0;
    [[nodiscard]] virtual bool exists(std::string_view path) const = 0;

    [[nodiscard]] virtual Result<std::string> readText(std::string_view path) const;
    // Writing is optional; read-only implementations return ErrorCode::Unsupported.
    [[nodiscard]] virtual Status writeText(std::string_view path, std::string_view content);

    // Human-readable location for error messages.
    [[nodiscard]] virtual std::string describe(std::string_view path) const { return std::string(path); }

    // Upper bound for a single file read; protects against hostile packages.
    static constexpr std::size_t kMaxFileSize = 64u * 1024u * 1024u;
};

// Files under a root directory on the local disk. Paths cannot escape the root.
class DirectoryFileSystem final : public IFileSystem {
public:
    explicit DirectoryFileSystem(std::filesystem::path root);

    [[nodiscard]] Result<std::vector<std::uint8_t>> readBinary(std::string_view path) const override;
    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] Status writeText(std::string_view path, std::string_view content) override;
    [[nodiscard]] std::string describe(std::string_view path) const override;

    [[nodiscard]] const std::filesystem::path& root() const { return m_root; }

private:
    std::filesystem::path m_root;
};

// In-memory files; used by tests and as a future package/staging container.
class MemoryFileSystem final : public IFileSystem {
public:
    void addFile(std::string_view path, std::string_view content);
    void addFile(std::string_view path, std::vector<std::uint8_t> content);

    [[nodiscard]] Result<std::vector<std::uint8_t>> readBinary(std::string_view path) const override;
    [[nodiscard]] bool exists(std::string_view path) const override;
    [[nodiscard]] Status writeText(std::string_view path, std::string_view content) override;

private:
    std::map<std::string, std::vector<std::uint8_t>, std::less<>> m_files;
};

} // namespace px
