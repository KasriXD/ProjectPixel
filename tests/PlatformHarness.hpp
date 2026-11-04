#pragma once

// A platform service on a free port with its data in a temporary folder,
// for tests (test_platform.cpp, test_matchmaking.cpp).
#include "PlatformService.hpp"
#include "px/online/PlatformClient.hpp"
#include "px/project/Package.hpp"

#include <doctest.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <random>

namespace pxtest {

inline std::filesystem::path tempDir(const char* tag) {
    static std::atomic<int> counter{0};
    std::random_device rd;
    const auto dir = std::filesystem::temp_directory_path() /
                     std::format("px-{}-{}-{}", tag, rd() % 1000000, counter.fetch_add(1));
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

struct TestPlatform {
    std::filesystem::path dir;
    std::unique_ptr<px::platform::PlatformService> service;

    explicit TestPlatform(bool rateLimits = false, std::uint16_t firstServerPort = 0,
                          const std::function<void(px::platform::ServiceConfig&)>& adjust = {}) {
        dir = tempDir("platform");
        px::platform::ServiceConfig config;
        config.dataDir = dir;
        config.port = 0;
        config.passwordHashing = {64, 1}; // fast for tests
        config.rateLimits = rateLimits;
        config.headlessExe = PX_HEADLESS_EXE;
        config.serverExe = PX_SERVER_EXE;
        config.firstServerPort = firstServerPort != 0 ? firstServerPort
                                                      : static_cast<std::uint16_t>(29000 + std::random_device{}() % 2000);
        config.serverPortCount = 20;
        config.threads = 16;
        if (adjust) adjust(config);
        auto created = px::platform::PlatformService::create(config);
        REQUIRE_MESSAGE(created.ok(), (created.ok() ? "" : created.error().message));
        service = std::move(created.value());
        auto started = service->start();
        REQUIRE_MESSAGE(started.ok(), (started.ok() ? "" : started.error().message));
    }
    ~TestPlatform() {
        service->stop();
        service.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    [[nodiscard]] px::online::PlatformClient client() const { return px::online::PlatformClient(service->url()); }

    // A logged-in client for a new account.
    [[nodiscard]] px::online::PlatformClient account(const std::string& name) const {
        auto c = client();
        auto r = c.registerAccount(name, "password-" + name);
        REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
        return c;
    }
};

inline std::vector<std::uint8_t> examplePackage(const char* name) {
    auto package = px::project::collectPackage(std::filesystem::path(PX_EXAMPLES_DIR) / name);
    REQUIRE(package.ok());
    return px::project::encodePackage(package.value());
}

inline bool contains(const std::string& text, std::string_view needle) { return text.find(needle) != std::string::npos; }

} // namespace pxtest
