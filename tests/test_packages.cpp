#include "px/project/Package.hpp"
#include "px/project/Project.hpp"
#include "px/runtime/GameLoader.hpp"
#if defined(PX_HAVE_PLATFORM)
#include "px/online/PackageCheck.hpp"
#endif

#include <doctest.h>

#include "px/audio/Sound.hpp"

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
    // Every sound is now QOA the engine wrote, and still plays.
    CHECK(report->sounds == 5);
    CHECK(report->soundSeconds > 15.0);
    for (const auto& entry : manifest->assets.entries()) {
        if (entry.type != assets::AssetType::Sound) continue;
        const auto* file = package.find(entry.path);
        REQUIRE(file != nullptr);
        REQUIRE(file->data.size() > 8);
        CHECK(std::string(file->data.begin(), file->data.begin() + 4) == "qoaf");
        auto info = audio::probeSound(file->data);
        REQUIRE(info.ok());
        CHECK(info->format == audio::SoundFormat::Qoa);
        CHECK(audio::decodeSound(file->data).ok());
    }
    auto json = online::reportToJson(report.value());
    auto back = online::reportFromJson(json);
    REQUIRE(back.ok());
    CHECK(back->images == report->images);
    CHECK(back->sounds == report->sounds);

    // Fonts: letters must be inside their image.
    const auto withFont = [&](int glyphX) {
        Package p = must(collectPackage(example("topdown")));
        auto fsCopy = p.toFileSystem();
        auto m = loadProjectManifest(*fsCopy);
        REQUIRE(m.ok());
        const auto* gem = m->assets.findByName("Gem");
        REQUIRE(gem != nullptr);
        for (auto& f : p.files) {
            if (f.path != "project.json") continue;
            std::string text(f.data.begin(), f.data.end());
            const auto at = text.find("\"assets\": [");
            REQUIRE(at != std::string::npos);
            text.insert(at + 11, std::format(R"({{"id": "aaaaaaaa-0000-4000-8000-0000000000f0", "type": "font",
                "name": "Tiny", "path": "fonts/tiny.font.json"}},)"));
            f.data.assign(text.begin(), text.end());
        }
        const std::string font = std::format(R"({{"format": "px.font", "formatVersion": 1, "texture": "{}",
            "size": 8, "lineHeight": 10, "ascent": 8, "glyphs": [[65, {}, 0, 8, 8, 0, 0, 8]]}})",
                                              gem->id.toString(), glyphX);
        p.files.push_back({"fonts/tiny.font.json", std::vector<std::uint8_t>(font.begin(), font.end())});
        return online::checkPackage(p);
    };
    auto fine = withFont(0);
    REQUIRE_MESSAGE(fine.ok(), (fine.ok() ? "" : fine.error().message));
    CHECK(fine->fonts == 1);
    auto outside = withFont(4000);
    REQUIRE_FALSE(outside.ok());
    CHECK(contains(outside.error().message, "outside"));

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
    Package badSound = must(collectPackage(example("platformer")));
    for (auto& f : badSound.files) {
        if (f.path.ends_with("coin.wav")) f.data.resize(30); // cut off inside its header
    }
    auto noSound = online::checkPackage(badSound);
    REQUIRE_FALSE(noSound.ok());
    CHECK(contains(noSound.error().message, "coin.wav"));
}
#endif
