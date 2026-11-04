#include "px/project/Package.hpp"
#include "px/project/Project.hpp"
#include "px/runtime/GameLoader.hpp"
#if defined(PX_HAVE_PLATFORM)
#include "px/online/PackageCheck.hpp"
#endif

#include <doctest.h>

#include <algorithm>

using namespace px;
using namespace px::project;

namespace {
bool contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

std::filesystem::path example(const char* name) { return std::filesystem::path(PX_EXAMPLES_DIR) / name; }

Package must(Result<Package> r) {
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    return std::move(r.value());
}

void put(std::vector<std::uint8_t>& out, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

// Builds package bytes by hand, for malformed inputs.
std::vector<std::uint8_t> rawPackage(const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<std::uint8_t> out = {'P', 'X', 'P', 'K'};
    put(out, 1, 2);
    put(out, 0, 2);
    put(out, files.size(), 4);
    for (const auto& [path, data] : files) {
        put(out, path.size(), 2);
        out.insert(out.end(), path.begin(), path.end());
        put(out, data.size(), 8);
    }
    for (const auto& f : files) out.insert(out.end(), f.second.begin(), f.second.end());
    return out;
}
} // namespace

TEST_CASE("Packages: a project packs into a package that plays the same") {
    const Package package = must(collectPackage(example("platformer")));
    CHECK(package.find("project.json") != nullptr);
    CHECK(package.find("scenes/level1.scene.json") != nullptr);
    CHECK(package.find("graphs/drift.graph.json") != nullptr);
    CHECK(package.find("tests/full_run.input") == nullptr); // only what the game uses
    CHECK(std::is_sorted(package.files.begin(), package.files.end(),
                         [](const PackageFile& a, const PackageFile& b) { return a.path < b.path; }));

    const auto bytes = encodePackage(package);
    CHECK(encodePackage(package) == bytes); // deterministic
    const Package back = must(decodePackage(bytes));
    REQUIRE(back.files.size() == package.files.size());
    for (std::size_t i = 0; i < back.files.size(); ++i) {
        CHECK(back.files[i].path == package.files[i].path);
        CHECK(back.files[i].data == package.files[i].data);
    }
    auto fs = back.toFileSystem();
    auto game = runtime::loadGame(*fs);
    REQUIRE(game.ok());
    CHECK(game->manifest.name == "Example Platformer");
}

TEST_CASE("Packages: malformed packages are rejected") {
    const std::string manifest = "{}";
    CHECK(decodePackage(rawPackage({{"project.json", manifest}})).ok());
    CHECK_FALSE(decodePackage(std::vector<std::uint8_t>{'P', 'X'}).ok());
    auto badMagic = rawPackage({{"project.json", manifest}});
    badMagic[0] = 'Z';
    CHECK_FALSE(decodePackage(badMagic).ok());
    auto badVersion = rawPackage({{"project.json", manifest}});
    badVersion[4] = 9;
    CHECK(contains(decodePackage(badVersion).error().message, "format 9"));
    CHECK_FALSE(decodePackage(rawPackage({{"a.png", "x"}})).ok());                                 // no project.json
    CHECK_FALSE(decodePackage(rawPackage({{"../evil.txt", "x"}, {"project.json", manifest}})).ok()); // escapes
    CHECK_FALSE(decodePackage(rawPackage({{"/abs.txt", "x"}, {"project.json", manifest}})).ok());
    CHECK_FALSE(decodePackage(rawPackage({{"a\\\\b.txt", "x"}, {"project.json", manifest}})).ok());
    CHECK_FALSE(decodePackage(rawPackage({{"project.json", manifest}, {"project.json", manifest}})).ok()); // twice
    CHECK_FALSE(decodePackage(rawPackage({{"z.txt", "x"}, {"project.json", manifest}})).ok());            // unsorted
    auto truncated = rawPackage({{"a.txt", "abc"}, {"project.json", manifest}});
    truncated.pop_back();
    CHECK_FALSE(decodePackage(truncated).ok());
    auto extra = rawPackage({{"project.json", manifest}});
    extra.push_back(0);
    CHECK_FALSE(decodePackage(extra).ok());
    // A claimed size far beyond the data does not allocate it.
    std::vector<std::uint8_t> huge = {'P', 'X', 'P', 'K'};
    put(huge, 1, 2);
    put(huge, 0, 2);
    put(huge, 1, 4);
    put(huge, 12, 2);
    huge.insert(huge.end(), {'p', 'r', 'o', 'j', 'e', 'c', 't', '.', 'j', 's', 'o', 'n'});
    put(huge, 1ull << 40, 8);
    CHECK_FALSE(decodePackage(huge).ok());
    // Random bytes never crash the decoder.
    std::uint32_t seed = 7;
    auto good = rawPackage({{"a.txt", "hello"}, {"project.json", manifest}});
    for (int i = 0; i < 2000; ++i) {
        auto mutated = good;
        seed = seed * 1664525u + 1013904223u;
        mutated[seed % mutated.size()] = static_cast<std::uint8_t>(seed >> 24);
        (void)decodePackage(mutated);
    }
}

#if defined(PX_HAVE_PLATFORM)
TEST_CASE("Packages: the platform's check validates, re-encodes images and simulates") {
    Package package = must(collectPackage(example("topdown")));
    auto report = online::checkPackage(package);
    REQUIRE_MESSAGE(report.ok(), (report.ok() ? "" : report.error().message));
    CHECK(report->name == "Example Top-down");
    CHECK(report->gameType == "topdown");
    CHECK(report->images > 0);
    CHECK(report->scenes == 1);
    CHECK(report->scriptErrors == 0);
    CHECK(report->spawnPoints >= 1);
    // Every image is now a PNG the engine wrote.
    auto fs = package.toFileSystem();
    auto manifest = loadProjectManifest(*fs);
    REQUIRE(manifest.ok());
    for (const auto& entry : manifest->assets.entries()) {
        if (entry.type != assets::AssetType::Texture) continue;
        const auto* file = package.find(entry.path);
        REQUIRE(file != nullptr);
        REQUIRE(file->data.size() > 8);
        CHECK(file->data[1] == 'P');
        CHECK(file->data[2] == 'N');
        CHECK(file->data[3] == 'G');
    }
    auto json = online::reportToJson(report.value());
    auto back = online::reportFromJson(json);
    REQUIRE(back.ok());
    CHECK(back->images == report->images);

    // A broken script or image is refused with a message naming it.
    Package broken = must(collectPackage(example("platformer")));
    for (auto& f : broken.files) {
        if (f.path == "scripts/coin.luau") f.data = {'l', 'o', 'c', 'a', 'l', ' ', '='};
    }
    auto bad = online::checkPackage(broken);
    REQUIRE_FALSE(bad.ok());
    CHECK(contains(bad.error().message, "coin.luau"));
    Package badImage = must(collectPackage(example("platformer")));
    for (auto& f : badImage.files) {
        if (f.path.ends_with("tiles.png")) f.data.resize(40);
    }
    CHECK_FALSE(online::checkPackage(badImage).ok());
}
#endif
