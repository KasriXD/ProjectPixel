#include "px/project/Package.hpp"

#include "px/project/Project.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <set>

namespace px::project {

namespace {
// "PXPK", format version (u16), reserved (u16), file count (u32); then per
// file: path length (u16), path, size (u64); then the file contents in the
// same order. Integers are little-endian.
constexpr std::uint8_t kMagic[4] = {'P', 'X', 'P', 'K'};

void putU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>(v & 0xFF));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void putU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
void putU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : m_bytes(bytes) {}
    [[nodiscard]] bool has(std::size_t n) const { return n <= m_bytes.size() - m_pos; }
    std::uint64_t uint(int bytes) {
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= std::uint64_t{m_bytes[m_pos + static_cast<std::size_t>(i)]} << (8 * i);
        m_pos += static_cast<std::size_t>(bytes);
        return v;
    }
    std::span<const std::uint8_t> take(std::size_t n) {
        auto s = m_bytes.subspan(m_pos, n);
        m_pos += n;
        return s;
    }
    [[nodiscard]] std::size_t position() const { return m_pos; }
    [[nodiscard]] std::size_t size() const { return m_bytes.size(); }

private:
    std::span<const std::uint8_t> m_bytes;
    std::size_t m_pos = 0;
};

Error invalid(std::string message) { return Error{ErrorCode::InvalidData, "Invalid game package: " + std::move(message)}; }
} // namespace

const PackageFile* Package::find(std::string_view path) const {
    const auto it = std::lower_bound(files.begin(), files.end(), path,
                                     [](const PackageFile& f, std::string_view p) { return f.path < p; });
    return it != files.end() && it->path == path ? &*it : nullptr;
}

std::size_t Package::totalBytes() const {
    std::size_t n = 0;
    for (const PackageFile& f : files) n += f.data.size();
    return n;
}

std::unique_ptr<MemoryFileSystem> Package::toFileSystem() const {
    auto fs = std::make_unique<MemoryFileSystem>();
    for (const PackageFile& f : files) fs->addFile(f.path, f.data);
    return fs;
}

Result<Package> collectPackage(const IFileSystem& files) {
    auto manifest = loadProjectManifest(files);
    if (!manifest) return manifest.error();
    std::set<std::string> paths{std::string(kProjectFileName)};
    for (const auto& entry : manifest->assets.entries()) {
        auto path = normalizeRelativePath(entry.path);
        if (!path) return path.error();
        paths.insert(std::move(path.value()));
    }
    Package package;
    std::size_t total = 0;
    for (const std::string& path : paths) {
        auto data = files.readBinary(path);
        if (!data) return makeError(ErrorCode::NotFound, "Cannot package the game: {}", data.error().message);
        total += data->size();
        if (total > kMaxPackageBytes) {
            return makeError(ErrorCode::LimitExceeded, "The game is larger than {} MiB", kMaxPackageBytes >> 20);
        }
        package.files.push_back({path, std::move(data.value())});
    }
    return package; // std::set iterates in sorted order
}

Result<Package> collectPackage(const std::filesystem::path& projectDir) {
    const DirectoryFileSystem fs(projectDir);
    return collectPackage(fs);
}

std::vector<std::uint8_t> encodePackage(const Package& package) {
    std::vector<std::uint8_t> out;
    out.reserve(package.totalBytes() + 16 + package.files.size() * 64);
    out.insert(out.end(), std::begin(kMagic), std::end(kMagic));
    putU16(out, kPackageFormatVersion);
    putU16(out, 0);
    putU32(out, static_cast<std::uint32_t>(package.files.size()));
    for (const PackageFile& f : package.files) {
        putU16(out, static_cast<std::uint16_t>(f.path.size()));
        out.insert(out.end(), f.path.begin(), f.path.end());
        putU64(out, f.data.size());
    }
    for (const PackageFile& f : package.files) out.insert(out.end(), f.data.begin(), f.data.end());
    return out;
}

Result<Package> decodePackage(std::span<const std::uint8_t> bytes) {
    if (bytes.size() > kMaxPackageBytes + (kMaxPackageFiles * (kMaxPackagePathLength + 10)) + 12) {
        return invalid("too large");
    }
    Reader r(bytes);
    if (!r.has(12)) return invalid("too short");
    const auto magic = r.take(4);
    if (!std::equal(magic.begin(), magic.end(), std::begin(kMagic))) return invalid("not a game package");
    const auto version = static_cast<std::uint16_t>(r.uint(2));
    if (version != kPackageFormatVersion) {
        return invalid(std::format("format {} is not supported (this engine reads format {})", version,
                                   kPackageFormatVersion));
    }
    (void)r.uint(2);
    const std::uint64_t count = r.uint(4);
    if (count == 0 || count > kMaxPackageFiles) return invalid("wrong number of files");
    struct Header {
        std::string path;
        std::uint64_t size;
    };
    std::vector<Header> headers;
    headers.reserve(static_cast<std::size_t>(count));
    std::uint64_t total = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        if (!r.has(2)) return invalid("truncated file list");
        const auto length = static_cast<std::size_t>(r.uint(2));
        if (length == 0 || length > kMaxPackagePathLength || !r.has(length + 8)) return invalid("bad file name");
        const auto raw = r.take(length);
        std::string path(raw.begin(), raw.end());
        auto normalized = normalizeRelativePath(path);
        if (!normalized || normalized.value() != path) return invalid(std::format("bad file name '{}'", path));
        const std::uint64_t size = r.uint(8);
        if (size > IFileSystem::kMaxFileSize) return invalid(std::format("'{}' is too large", path));
        total += size;
        if (total > kMaxPackageBytes) return invalid("too large");
        if (!headers.empty() && !(headers.back().path < path)) {
            return invalid("files are not sorted or appear twice");
        }
        headers.push_back({std::move(path), size});
    }
    if (r.size() - r.position() != total) return invalid("the file contents do not match the file list");
    Package package;
    package.files.reserve(headers.size());
    for (Header& h : headers) {
        const auto data = r.take(static_cast<std::size_t>(h.size));
        package.files.push_back({std::move(h.path), std::vector<std::uint8_t>(data.begin(), data.end())});
    }
    if (package.find(kProjectFileName) == nullptr) return invalid("project.json is missing");
    return package;
}

Result<Package> readPackageFile(const std::filesystem::path& file) {
    auto bytes = readLocalFile(file, kMaxPackageBytes * 2);
    if (!bytes) return bytes.error();
    return decodePackage(bytes.value());
}

Status writePackageFile(const std::filesystem::path& file, const Package& package) {
    const auto bytes = encodePackage(package);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        return makeError(ErrorCode::IoError, "Cannot write {}", file.string());
    }
    return {};
}

} // namespace px::project
