#include <doctest.h>

#include "px/core/Color.hpp"
#include "px/core/FileSystem.hpp"
#include "px/core/Guid.hpp"
#include "px/core/Keys.hpp"
#include "px/core/Log.hpp"
#include "px/core/Math.hpp"
#include "px/core/Result.hpp"
#include "px/core/Time.hpp"

#include <filesystem>
#include <fstream>
#include <unordered_set>

using namespace px;

TEST_CASE("Result holds a value or an error") {
    Result<int> ok = 42;
    CHECK(ok.ok());
    CHECK(ok.value() == 42);

    Result<int> bad = makeError(ErrorCode::NotFound, "missing {}", "thing");
    CHECK_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::NotFound);
    CHECK(bad.error().message == "missing thing");

    Status s;
    CHECK(s.ok());
    Status failed = Error{ErrorCode::IoError, "disk"};
    CHECK_FALSE(failed.ok());
}

TEST_CASE("Guid generation, formatting and parsing") {
    const Guid a = Guid::generate();
    CHECK_FALSE(a.isNil());
    const std::string text = a.toString();
    CHECK(text.size() == 36);
    CHECK(text[14] == '4'); // version 4
    const auto parsed = Guid::parse(text);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == a);

    const auto bare = Guid::parse("0123456789abcdef0123456789ABCDEF");
    REQUIRE(bare.has_value());
    CHECK(bare->toString() == "01234567-89ab-cdef-0123-456789abcdef");

    CHECK_FALSE(Guid::parse("").has_value());
    CHECK_FALSE(Guid::parse("not-a-guid").has_value());
    CHECK_FALSE(Guid::parse("01234567-89ab-cdef-0123-456789abcde").has_value()); // too short
    CHECK_FALSE(Guid::parse("0123456789ab-cdef-0123-456789abcdef0").has_value()); // misplaced hyphen
    CHECK_FALSE(Guid::parse("01234567-89ab-cdef-0123-456789abcdeg").has_value()); // bad digit

    std::unordered_set<Guid> unique;
    for (int i = 0; i < 1000; ++i) unique.insert(Guid::generate());
    CHECK(unique.size() == 1000);
}

TEST_CASE("Color hex parsing round-trips") {
    const auto c = Color::parseHex("#FF8000");
    REQUIRE(c.has_value());
    CHECK(*c == Color{255, 128, 0, 255});
    CHECK(c->toHex() == "#ff8000");
    const auto withAlpha = Color::parseHex("10203040");
    REQUIRE(withAlpha.has_value());
    CHECK(withAlpha->a == 0x40);
    CHECK(withAlpha->toHex() == "#10203040");
    CHECK_FALSE(Color::parseHex("#12345").has_value());
    CHECK_FALSE(Color::parseHex("#zzzzzz").has_value());
}

TEST_CASE("Rect math") {
    const Rect r = Rect::fromCenter({10, 10}, {4, 6});
    CHECK(r == Rect{8, 7, 4, 6});
    CHECK(r.center() == Vec2{10, 10});
    CHECK(r.intersects(Rect{11, 12, 5, 5}));
    CHECK_FALSE(r.intersects(Rect{12, 7, 1, 1})); // touching edge only
    CHECK(unite(Rect{0, 0, 1, 1}, Rect{4, 5, 1, 1}) == Rect{0, 0, 5, 6});
    CHECK(approach(0.0f, 10.0f, 3.0f) == doctest::Approx(3.0f));
    CHECK(approach(9.0f, 10.0f, 3.0f) == doctest::Approx(10.0f));
    CHECK(approach(-5.0f, 0.0f, 10.0f) == doctest::Approx(0.0f));
}

TEST_CASE("Key names") {
    CHECK(keyName(Key::Space) == "Space");
    CHECK(keyFromName("space") == Key::Space);
    CHECK(keyFromName("LEFT") == Key::Left);
    CHECK(keyFromName("7") == Key::Num7);
    CHECK(keyFromName("F12") == Key::F12);
    CHECK(keyFromName("NoSuchKey") == Key::Unknown);
    for (std::size_t i = 1; i < kKeyCount; ++i) {
        const auto key = static_cast<Key>(i);
        CHECK(keyFromName(keyName(key)) == key);
    }
}

TEST_CASE("FixedStepClock converts frame time into whole steps") {
    FixedStepClock clock(0.01, 5);
    CHECK(clock.advance(0.005) == 0);
    CHECK(clock.advance(0.006) == 1);
    CHECK(clock.advance(0.025) == 2);
    CHECK(clock.alpha() == doctest::Approx(0.6).epsilon(0.01));
    // A long stall is capped and the backlog dropped.
    CHECK(clock.advance(10.0) == 5);
    CHECK(clock.advance(0.0) == 0);
}

TEST_CASE("Project paths are confined to the project") {
    CHECK(normalizeRelativePath("a/./b//c.png").value() == "a/b/c.png");
    CHECK(normalizeRelativePath("scenes/level1.scene.json").value() == "scenes/level1.scene.json");
    CHECK(normalizeRelativePath("../secret").error().code == ErrorCode::AccessDenied);
    CHECK(normalizeRelativePath("a/../../b").error().code == ErrorCode::AccessDenied);
    CHECK(normalizeRelativePath("/etc/passwd").error().code == ErrorCode::AccessDenied);
    CHECK(normalizeRelativePath("C:/Windows").error().code == ErrorCode::AccessDenied);
    CHECK(normalizeRelativePath("a\\b").error().code == ErrorCode::InvalidArgument);
    CHECK(normalizeRelativePath("").error().code == ErrorCode::InvalidArgument);
    CHECK(normalizeRelativePath("./.").error().code == ErrorCode::InvalidArgument);
}

TEST_CASE("MemoryFileSystem reads and writes") {
    MemoryFileSystem fs;
    fs.addFile("dir/file.txt", std::string_view("hello"));
    CHECK(fs.exists("dir/file.txt"));
    CHECK(fs.exists("dir/./file.txt"));
    CHECK(fs.readText("dir/file.txt").value() == "hello");
    CHECK(fs.readText("missing.txt").error().code == ErrorCode::NotFound);
    CHECK(fs.writeText("out.txt", "data").ok());
    CHECK(fs.readText("out.txt").value() == "data");
}

TEST_CASE("DirectoryFileSystem stays inside its root") {
    namespace fsys = std::filesystem;
    const fsys::path root = fsys::temp_directory_path() / ("px_fs_test_" + Guid::generate().toString());
    fsys::create_directories(root / "inner");
    DirectoryFileSystem fs(root / "inner");

    REQUIRE(fs.writeText("sub/a.txt", "abc").ok());
    CHECK(fs.readText("sub/a.txt").value() == "abc");
    CHECK(fs.readText("../outside.txt").error().code == ErrorCode::AccessDenied);
    CHECK(fs.readText("nope.txt").error().code == ErrorCode::NotFound);

    // A symlink pointing outside the root must not be readable.
    {
        std::ofstream(root / "outside.txt") << "secret";
    }
    std::error_code ec;
    fsys::create_symlink(root / "outside.txt", root / "inner" / "link.txt", ec);
    if (!ec) {
        CHECK(fs.readText("link.txt").error().code == ErrorCode::AccessDenied);
    }
    fsys::remove_all(root, ec);
}

TEST_CASE("Log sinks receive messages at or above the minimum level") {
    std::vector<std::string> captured;
    const auto previous = log::minLevel();
    log::setMinLevel(log::Level::Info);
    log::setConsoleOutput(false);
    const int handle = log::addSink([&](log::Level, std::string_view msg) { captured.emplace_back(msg); });
    log::debug("hidden {}", 1);
    log::info("shown {}", 2);
    log::removeSink(handle);
    log::info("after removal");
    log::setConsoleOutput(true);
    log::setMinLevel(previous);
    REQUIRE(captured.size() == 1);
    CHECK(captured[0] == "shown 2");
}
