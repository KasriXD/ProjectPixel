#include "px/core/FileSystem.hpp"

#include <fstream>
#include <system_error>

namespace px {

Result<std::string> normalizeRelativePath(std::string_view path) {
    if (path.empty()) {
        return makeError(ErrorCode::InvalidArgument, "Path is empty");
    }
    if (path.find('\\') != std::string_view::npos) {
        return makeError(ErrorCode::InvalidArgument, "Path '{}' uses '\\'; use '/' as the separator", path);
    }
    if (path.front() == '/') {
        return makeError(ErrorCode::AccessDenied, "Path '{}' must be relative to the project folder", path);
    }
    if (path.find(':') != std::string_view::npos) {
        return makeError(ErrorCode::AccessDenied, "Path '{}' may not contain ':' (drive letters and URLs are not allowed)", path);
    }
    if (path.find('\0') != std::string_view::npos) {
        return makeError(ErrorCode::InvalidArgument, "Path contains a NUL character");
    }

    std::string out;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string_view segment = path.substr(start, end - start);
        if (segment == "..") {
            return makeError(ErrorCode::AccessDenied, "Path '{}' may not contain '..'", path);
        }
        if (!segment.empty() && segment != ".") {
            if (!out.empty()) out += '/';
            out += segment;
        }
        start = end + 1;
    }
    if (out.empty()) {
        return makeError(ErrorCode::InvalidArgument, "Path '{}' does not name a file", path);
    }
    return out;
}

Result<std::string> IFileSystem::readText(std::string_view path) const {
    auto bytes = readBinary(path);
    if (!bytes) return bytes.error();
    const auto& data = bytes.value();
    return std::string(reinterpret_cast<const char*>(data.data()), data.size());
}

Status IFileSystem::writeText(std::string_view path, std::string_view) {
    return makeError(ErrorCode::Unsupported, "Cannot write '{}': file system is read-only", path);
}

// --- DirectoryFileSystem ----------------------------------------------------

namespace {

// True if `full` (after resolving symlinks) is inside `root`. Prevents a
// symlink inside an untrusted project from pointing at files outside it.
bool resolvesInsideRoot(const std::filesystem::path& root, const std::filesystem::path& full) {
    std::error_code ec;
    const auto canonicalRoot = std::filesystem::weakly_canonical(root, ec);
    if (ec) return false;
    const auto canonicalFull = std::filesystem::weakly_canonical(full, ec);
    if (ec) return false;
    auto rootIt = canonicalRoot.begin();
    auto fullIt = canonicalFull.begin();
    for (; rootIt != canonicalRoot.end(); ++rootIt, ++fullIt) {
        if (rootIt->empty()) continue; // trailing separator
        if (fullIt == canonicalFull.end() || *rootIt != *fullIt) return false;
    }
    return true;
}

} // namespace

DirectoryFileSystem::DirectoryFileSystem(std::filesystem::path root) : m_root(std::move(root)) {}

std::string DirectoryFileSystem::describe(std::string_view path) const {
    return (m_root / std::filesystem::path(std::string(path))).generic_string();
}

Result<std::vector<std::uint8_t>> DirectoryFileSystem::readBinary(std::string_view path) const {
    auto normalized = normalizeRelativePath(path);
    if (!normalized) return normalized.error();
    const auto full = m_root / std::filesystem::path(normalized.value());

    std::error_code ec;
    if (!std::filesystem::is_regular_file(full, ec)) {
        return makeError(ErrorCode::NotFound, "File not found: {}", full.generic_string());
    }
    if (!resolvesInsideRoot(m_root, full)) {
        return makeError(ErrorCode::AccessDenied, "{} resolves outside the project folder", full.generic_string());
    }
    const auto size = std::filesystem::file_size(full, ec);
    if (ec) {
        return makeError(ErrorCode::IoError, "Cannot read size of {}: {}", full.generic_string(), ec.message());
    }
    if (size > kMaxFileSize) {
        return makeError(ErrorCode::LimitExceeded, "{} is {} bytes; the limit is {} bytes", full.generic_string(), size,
                         kMaxFileSize);
    }
    std::ifstream in(full, std::ios::binary);
    if (!in) {
        return makeError(ErrorCode::IoError, "Cannot open {}", full.generic_string());
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (size > 0 && !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size))) {
        return makeError(ErrorCode::IoError, "Failed while reading {}", full.generic_string());
    }
    return data;
}

bool DirectoryFileSystem::exists(std::string_view path) const {
    auto normalized = normalizeRelativePath(path);
    if (!normalized) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(m_root / std::filesystem::path(normalized.value()), ec);
}

Status DirectoryFileSystem::writeText(std::string_view path, std::string_view content) {
    auto normalized = normalizeRelativePath(path);
    if (!normalized) return normalized.error();
    const auto full = m_root / std::filesystem::path(normalized.value());
    std::error_code ec;
    std::filesystem::create_directories(full.parent_path(), ec);
    if (ec) {
        return makeError(ErrorCode::IoError, "Cannot create folder for {}: {}", full.generic_string(), ec.message());
    }
    // Write to a temporary file first so a crash never leaves a half-written file.
    auto temp = full;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return makeError(ErrorCode::IoError, "Cannot open {} for writing", temp.generic_string());
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!out) return makeError(ErrorCode::IoError, "Failed while writing {}", temp.generic_string());
    }
    std::filesystem::rename(temp, full, ec);
    if (ec) {
        return makeError(ErrorCode::IoError, "Cannot replace {}: {}", full.generic_string(), ec.message());
    }
    return {};
}

// --- MemoryFileSystem -------------------------------------------------------

void MemoryFileSystem::addFile(std::string_view path, std::string_view content) {
    addFile(path, std::vector<std::uint8_t>(content.begin(), content.end()));
}

void MemoryFileSystem::addFile(std::string_view path, std::vector<std::uint8_t> content) {
    auto normalized = normalizeRelativePath(path);
    m_files[normalized ? normalized.value() : std::string(path)] = std::move(content);
}

Result<std::vector<std::uint8_t>> MemoryFileSystem::readBinary(std::string_view path) const {
    auto normalized = normalizeRelativePath(path);
    if (!normalized) return normalized.error();
    const auto it = m_files.find(normalized.value());
    if (it == m_files.end()) {
        return makeError(ErrorCode::NotFound, "File not found: {}", normalized.value());
    }
    return it->second;
}

bool MemoryFileSystem::exists(std::string_view path) const {
    auto normalized = normalizeRelativePath(path);
    return normalized && m_files.contains(normalized.value());
}

Status MemoryFileSystem::writeText(std::string_view path, std::string_view content) {
    auto normalized = normalizeRelativePath(path);
    if (!normalized) return normalized.error();
    m_files[normalized.value()] = std::vector<std::uint8_t>(content.begin(), content.end());
    return {};
}

} // namespace px
