#pragma once

// A platform service on a free port with its data in a temporary folder,
// for tests (test_platform.cpp, test_matchmaking.cpp).
#include "PlatformService.hpp"
#include "px/net/GameClient.hpp"
#include "px/net/Udp.hpp"
#include "px/net/WebSocket.hpp"
#include "px/online/PlatformClient.hpp"
#include "px/project/Package.hpp"
#include "px/runtime/GameLoader.hpp"

#include <doctest.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <chrono>
#include <random>
#include <thread>

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


// A player connected to a platform game server over UDP, in real time.
struct OnlinePlayer {
    std::unique_ptr<px::net::GameClient> client;
    px::runtime::LoadedGame game;
    std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
    [[nodiscard]] double clock() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    }

    OnlinePlayer(px::online::PlatformClient& platform, const px::online::JoinTicket& ticket, bool webSocket = false) {
        auto bytes = platform.downloadPackage(ticket.packageHash);
        REQUIRE(bytes.ok());
        auto package = px::project::decodePackage(bytes.value());
        REQUIRE(package.ok());
        auto fs = package->toFileSystem();
        auto loaded = px::runtime::loadGame(*fs);
        REQUIRE(loaded.ok());
        game = std::move(loaded.value());
        auto endpoint = webSocket ? px::net::connectWebSocket(ticket.webSocket) : px::net::connectUdp(ticket.host, ticket.port);
        REQUIRE(endpoint.ok());
        px::net::ClientOptions options;
        options.token = ticket.token;
        auto created = px::net::GameClient::create(game, std::move(endpoint.value()), options);
        REQUIRE(created.ok());
        client = std::move(created.value());
    }

    // Runs in real time for `seconds` (or until disconnected), holding `actions`.
    void play(double seconds, std::initializer_list<std::string_view> actions = {}) {
        std::uint32_t held = 0;
        const px::runtime::ActionSet set(game.manifest.input);
        for (auto a : actions) held |= 1u << set.find(a);
        const double until = clock() + seconds;
        auto next = std::chrono::steady_clock::now();
        while (true) {
            const double now = clock();
            if (now > until || client->status() == px::net::GameClient::Status::Disconnected) break;
            client->receive(now);
            if (client->status() == px::net::GameClient::Status::Playing) client->tick({held, 0});
            client->updateView(now);
            next += std::chrono::milliseconds(16);
            std::this_thread::sleep_until(next);
        }
    }
    bool waitUntilPlaying(double seconds = 10.0) {
        const double until = clock() + seconds;
        while (client->status() != px::net::GameClient::Status::Playing && clock() < until &&
               client->status() != px::net::GameClient::Status::Disconnected) {
            client->receive(clock());
            client->tick({});
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return client->status() == px::net::GameClient::Status::Playing;
    }
};

// Waits for `condition`, keeping the connected players' clients running
// meanwhile (a client that stops answering is dropped by the server).
template <class F>
bool eventually(F&& condition, std::initializer_list<OnlinePlayer*> players = {}, double seconds = 20.0) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < end) {
        if (condition()) return true;
        if (players.size() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        for (OnlinePlayer* p : players) p->play(0.05);
    }
    return condition();
}


} // namespace pxtest
