#pragma once

#include "Mail.hpp"

#include "Fleet.hpp"
#include "RateLimiter.hpp"
#include "px/core/Result.hpp"
#include "px/crypto/Crypto.hpp"
#include "px/online/PlatformTypes.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace httplib {
class Server;
}

// The platform service (px-platform): accounts, the game catalog,
// publishing, joining game servers, saved player data and moderation,
// over an HTTP API (docs/platform.md).
namespace px::platform {

class Database;

struct ServiceConfig {
    std::filesystem::path dataDir;          // database, packages, logs, the platform key
    std::string bindAddress = "127.0.0.1";
    int port = online::kDefaultPlatformPort; // 0: any free port (tests)
    std::string name = "Project Pixel (local)";
    std::string publicHost = "127.0.0.1";   // how players reach game servers
    std::string platformUrl;                // how game servers reach the platform; default from the port
    std::filesystem::path headlessExe;      // px-headless: checks uploaded games
    std::filesystem::path serverExe;        // px-server: runs games
    std::uint16_t firstServerPort = 27960;
    std::uint16_t serverPortCount = 100;
    std::size_t maxServers = 20;
    std::size_t serverMemoryMiB = 1024;
    int serverCpuPercent = 100;             // a server using more than this for 30 s is stopped (0: no limit)
    bool sandbox = true;                    // game servers and checks lock themselves down (Linux)
    std::size_t checkMemoryMiB = 2048;
    double checkTimeoutSeconds = 60.0;
    int checkTicks = 120;
    crypto::PasswordHashing passwordHashing;
    bool rateLimits = true;
    int sessionDays = 30;
    std::size_t maxGamesPerUser = 50;
    // HTTPS (docs/platform.md#https): a second listener with the same API.
    // Game servers keep using the plain HTTP port on this computer.
    int httpsPort = 0;                      // 0: no HTTPS
    std::string httpsBindAddress;           // default: bindAddress
    std::filesystem::path tlsCert;          // PEM certificate (chain)
    std::filesystem::path tlsKey;           // PEM private key
    // The browser player (docs/platform.md#browsers): a folder with
    // px-player.js, px-player.wasm and px-player.data from the WebAssembly
    // build, served at /play/player/. Empty: no browser play.
    std::filesystem::path webPlayerDir;
    std::size_t threads = 32; // joins wait for new game servers, which call back on other threads
    // Accounts (docs/accounts.md): email for address checks and password
    // resets, and where the links in it point.
    MailConfig mail;
    std::string publicUrl;             // what people open; default: the HTTPS address, else the HTTP one
    bool requireVerifiedEmail = false; // publishing needs a confirmed email address
};

class PlatformService {
public:
    [[nodiscard]] static Result<std::unique_ptr<PlatformService>> create(ServiceConfig config);
    ~PlatformService();
    PlatformService(const PlatformService&) = delete;
    PlatformService& operator=(const PlatformService&) = delete;

    // Starts listening (in background threads); returns once it accepts connections.
    [[nodiscard]] Status start();
    void stop();
    [[nodiscard]] int port() const { return m_port; }
    [[nodiscard]] std::string url() const;
    // "https://host:port" when HTTPS is on, else empty.
    [[nodiscard]] std::string httpsUrl() const;
    [[nodiscard]] int httpsPort() const { return m_httpsPort; }

    // Makes an account an administrator or a normal user again (command line).
    [[nodiscard]] Status setRole(std::string_view username, online::Role role);
    [[nodiscard]] const crypto::PublicKey& publicKey() const { return m_key.publicKey; }
    [[nodiscard]] Fleet& fleet() { return *m_fleet; }
    [[nodiscard]] Mailer& mailer() { return *m_mailer; }
    // A password reset link for an account (operators: px-platform --reset-link NAME), valid for an hour.
    [[nodiscard]] Result<std::string> passwordResetLink(std::string_view username);
    // The address people open: links in emails point there.
    [[nodiscard]] std::string publicUrl() const;

    // Internal: shared by the service's handler groups (ServiceContext.hpp).
    struct Context;

private:
    explicit PlatformService(ServiceConfig config);
    [[nodiscard]] Status initialize();
    void registerRoutes(httplib::Server& http);
    void migrate();
    void migrateTo1();
    void migrateTo2();
    void migrateTo3();

    struct Impl;
    ServiceConfig m_config;
    std::unique_ptr<Database> m_db;
    std::mutex m_dbMutex;
    crypto::SigningKey m_key;
    std::unique_ptr<Fleet> m_fleet;
    std::unique_ptr<Mailer> m_mailer;
    std::unique_ptr<httplib::Server> m_http;
    std::unique_ptr<httplib::Server> m_https; // an httplib::SSLServer
    std::thread m_httpsListener;
    int m_httpsPort = 0;
    RateLimiter m_limits;
    std::thread m_listener;
    std::thread m_maintenance;
    std::atomic<bool> m_running{false};
    int m_port = 0;
    std::unique_ptr<Impl> m_impl;
    friend struct Impl;
    friend struct Context;
};

} // namespace px::platform
