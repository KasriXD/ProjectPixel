#include <doctest.h>

#include "px/assets/AssetRegistry.hpp"
#include "px/assets/Image.hpp"
#include "px/core/FileSystem.hpp"

#include <cstring>

using namespace px;
using namespace px::assets;

namespace {

AssetId idFrom(const char* text) { return AssetId{*Guid::parse(text)}; }

// Minimal valid 1x1 RGBA PNG (red pixel).
const std::vector<std::uint8_t> kRedPixelPng = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
    0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
    0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x56, 0xC7, 0x2F, 0x0D, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

} // namespace

TEST_CASE("AssetRegistry rejects duplicate ids and unsafe paths") {
    AssetRegistry reg;
    const AssetId a = idFrom("aaaaaaaa-0000-4000-8000-000000000001");
    CHECK(reg.add({a, AssetType::Texture, "tiles", "assets/./tiles.png", TextureFilter::Nearest}).ok());
    REQUIRE(reg.find(a) != nullptr);
    CHECK(reg.find(a)->path == "assets/tiles.png");
    CHECK(reg.findByName("tiles") == reg.find(a));

    CHECK_FALSE(reg.add({a, AssetType::Texture, "dup", "x.png", TextureFilter::Nearest}).ok());
    CHECK_FALSE(reg.add({AssetId{}, AssetType::Texture, "nil", "x.png", TextureFilter::Nearest}).ok());
    const auto escape = reg.add({idFrom("aaaaaaaa-0000-4000-8000-000000000002"), AssetType::Texture, "bad",
                                 "../x.png", TextureFilter::Nearest});
    CHECK(escape.error().code == ErrorCode::AccessDenied);
    CHECK(reg.size() == 1);
}

TEST_CASE("decodeImage decodes PNG to RGBA") {
    auto image = decodeImage(kRedPixelPng, "red.png");
    REQUIRE(image.ok());
    CHECK(image->width == 1);
    CHECK(image->height == 1);
    REQUIRE(image->pixels.size() == 4);
    CHECK(image->pixels[0] == 255);
    CHECK(image->pixels[1] == 0);
    CHECK(image->pixels[2] == 0);
    CHECK(image->pixels[3] == 255);
}

TEST_CASE("decodeImage rejects garbage and oversized images before decoding") {
    const std::vector<std::uint8_t> garbage = {1, 2, 3, 4, 5};
    auto bad = decodeImage(garbage, "garbage.png");
    CHECK(bad.error().code == ErrorCode::InvalidData);
    CHECK(bad.error().message.find("garbage.png") != std::string::npos);

    CHECK(decodeImage({}, "empty.png").error().code == ErrorCode::InvalidData);

    auto limited = decodeImage(kRedPixelPng, "red.png", ImageLimits{0, 0});
    CHECK(limited.error().code == ErrorCode::LimitExceeded);

    // Header claims 100000 x 100000 pixels: rejected from the header alone.
    std::vector<std::uint8_t> huge = kRedPixelPng;
    const std::uint8_t dims[8] = {0x00, 0x01, 0x86, 0xA0, 0x00, 0x01, 0x86, 0xA0};
    std::memcpy(&huge[16], dims, 8);
    auto big = decodeImage(huge, "huge.png");
    CHECK_FALSE(big.ok());
}

TEST_CASE("Example project textures decode") {
    DirectoryFileSystem fs(std::string(PX_EXAMPLES_DIR) + "/platformer");
    for (const char* path : {"assets/tiles.png", "assets/player.png", "assets/coin.png", "assets/scenery.png"}) {
        auto bytes = fs.readBinary(path);
        REQUIRE_MESSAGE(bytes.ok(), path);
        auto image = decodeImage(bytes.value(), path);
        REQUIRE_MESSAGE(image.ok(), path);
        CHECK(image->width > 0);
        CHECK(image->height > 0);
    }
}
