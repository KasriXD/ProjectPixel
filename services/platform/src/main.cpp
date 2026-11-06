// px-platform: the platform service (docs/platform.md). Accounts, the game
// catalog, publishing, game servers, saved data and moderation, over HTTP.
//
//   px-platform [--data DIR] [--port N] [--bind ADDRESS] [--public-host HOST]
//               [--platform-url URL] [--name NAME] [--server-ports FIRST:COUNT]
//               [--max-servers N] [--headless FILE] [--server FILE] [--no-rate-limits]
//   px-platform --data DIR --make-admin NAME
//   px-platform --data DIR --dev-certificate HOST[,HOST...]
//   HTTPS: --https-port N --tls-cert FILE --tls-key FILE [--https-bind ADDRESS]
//   Browsers: --web-player DIR (the WebAssembly player, served at /play)
#include "Certificates.hpp"
#include "PlatformService.hpp"
#include "px/core/Log.hpp"
#include "px/core/Version.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace px;

namespace {
std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

void usage() {
    std::fprintf(stderr,
                 "%s platform service %s\n"
                 "usage: px-platform [--data DIR] [--port N] [--bind ADDRESS] [--public-host HOST]\n"
                 "                   [--platform-url URL] [--name NAME] [--server-ports FIRST:COUNT]\n"
                 "                   [--max-servers N] [--headless FILE] [--server FILE] [--no-rate-limits]\n"
                 "                   [--https-port N --tls-cert FILE --tls-key FILE [--https-bind ADDRESS]]\n"
                 "                   [--web-player DIR]   (px-player.js/.wasm/.data: play in browsers at /play)\n"
                 "                   [--server-memory MIB] [--server-cpu PERCENT] [--no-sandbox]\n"
                 "                   [--public-url URL] [--require-verified-email]\n"
                 "                   [--smtp smtps://HOST:465|smtp://HOST:587 --mail-from ADDRESS [--smtp-user NAME]\n"
                 "                    [--smtp-ca FILE]] | [--mail-dir DIR]   (email: address checks, password\n"
                 "                    resets; the SMTP password comes from PX_SMTP_PASSWORD; docs/accounts.md)\n"
                 "                        (each game server: memory, share of a processor before it is stopped\n"
                 "                         (0: no limit), and whether it locks itself down; docs/security.md)\n"
                 "       px-platform --data DIR --make-admin NAME\n"
                 "       px-platform --data DIR --reset-link NAME [--public-url URL]   (a password reset link, 1 hour)\n"
                 "       px-platform --data DIR --dev-certificate HOST[,HOST...]   (self-signed, for testing HTTPS)\n"
                 "Defaults: --data ./platform-data --port 8730 --bind 127.0.0.1 --server-ports 27960:100\n"
                 "          --server-memory 1024 --server-cpu 100\n",
                 version::productName, version::string);
}

// px-headless and px-server: next to this program (installed), else where
// the build put them.
std::filesystem::path findTool(const std::filesystem::path& self, const char* name, const char* buildPath) {
#if defined(_WIN32)
    const std::string file = std::string(name) + ".exe";
#else
    const std::string file = name;
#endif
    const auto beside = self.parent_path() / file;
    std::error_code ec;
    if (std::filesystem::exists(beside, ec)) return beside;
    return buildPath;
}
} // namespace

int main(int argc, char** argv) {
    platform::ServiceConfig config;
    config.dataDir = "platform-data";
    std::string makeAdmin;
    std::string resetLink;
    std::string devCertificate;
    const std::filesystem::path self = std::filesystem::absolute(argv[0]);
    config.headlessExe = findTool(self, "px-headless", PX_HEADLESS_PATH);
    config.serverExe = findTool(self, "px-server", PX_SERVER_PATH);
    // Installed next to this program: web-player/px-player.js and friends.
    if (std::error_code ec; std::filesystem::is_regular_file(self.parent_path() / "web-player" / "px-player.js", ec)) {
        config.webPlayerDir = self.parent_path() / "web-player";
    }
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", arg.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        try {
            if (arg == "--data") {
                config.dataDir = value();
            } else if (arg == "--port") {
                config.port = std::stoi(value());
            } else if (arg == "--bind") {
                config.bindAddress = value();
            } else if (arg == "--public-host") {
                config.publicHost = value();
            } else if (arg == "--platform-url") {
                config.platformUrl = value();
            } else if (arg == "--name") {
                config.name = value();
            } else if (arg == "--server-ports") {
                const std::string v = value();
                const auto colon = v.find(':');
                config.firstServerPort = static_cast<std::uint16_t>(std::stoi(v.substr(0, colon)));
                if (colon != std::string::npos) config.serverPortCount = static_cast<std::uint16_t>(std::stoi(v.substr(colon + 1)));
            } else if (arg == "--max-servers") {
                config.maxServers = static_cast<std::size_t>(std::stoi(value()));
            } else if (arg == "--headless") {
                config.headlessExe = value();
            } else if (arg == "--server") {
                config.serverExe = value();
            } else if (arg == "--no-rate-limits") {
                config.rateLimits = false;
            } else if (arg == "--server-memory") {
                config.serverMemoryMiB = static_cast<std::size_t>(std::stoul(value()));
            } else if (arg == "--server-cpu") {
                config.serverCpuPercent = std::stoi(value());
            } else if (arg == "--no-sandbox") {
                config.sandbox = false;
            } else if (arg == "--public-url") {
                config.publicUrl = value();
            } else if (arg == "--smtp") {
                config.mail.server = value();
            } else if (arg == "--smtp-user") {
                config.mail.user = value();
            } else if (arg == "--smtp-ca") {
                config.mail.caFile = value();
            } else if (arg == "--mail-from") {
                config.mail.from = value();
            } else if (arg == "--mail-dir") {
                config.mail.outbox = value();
            } else if (arg == "--require-verified-email") {
                config.requireVerifiedEmail = true;
            } else if (arg == "--reset-link") {
                resetLink = value();
            } else if (arg == "--make-admin") {
                makeAdmin = value();
            } else if (arg == "--https-port") {
                config.httpsPort = std::stoi(value());
            } else if (arg == "--https-bind") {
                config.httpsBindAddress = value();
            } else if (arg == "--tls-cert") {
                config.tlsCert = value();
            } else if (arg == "--tls-key") {
                config.tlsKey = value();
            } else if (arg == "--web-player") {
                config.webPlayerDir = value();
            } else if (arg == "--dev-certificate") {
                devCertificate = value();
            } else {
                usage();
                return 2;
            }
        } catch (const std::exception&) {
            std::fprintf(stderr, "bad value for %s\n", arg.c_str());
            return 2;
        }
    }

    if (!devCertificate.empty()) {
        std::vector<std::string> hosts;
        std::size_t start = 0;
        while (start <= devCertificate.size()) {
            const auto comma = devCertificate.find(',', start);
            hosts.push_back(devCertificate.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        const auto cert = config.dataDir / "tls" / "dev-cert.pem";
        const auto key = config.dataDir / "tls" / "dev-key.pem";
        if (auto made = platform::makeDevCertificate(hosts, cert, key); !made) {
            log::error("{}", made.error().message);
            return 1;
        }
        log::info("Wrote {} and {}. Start with --https-port 8731 --tls-cert {} --tls-key {}; clients trust it with "
                  "PX_PLATFORM_CA={}",
                  cert.string(), key.string(), cert.string(), key.string(), cert.string());
        return 0;
    }
    if (const char* password = std::getenv("PX_SMTP_PASSWORD")) config.mail.password = password;
    if (!config.mail.server.empty() && config.mail.from.empty()) {
        log::error("--smtp needs --mail-from ADDRESS (the sender of the platform's email)");
        return 2;
    }
    if (config.httpsPort != 0 && (config.tlsCert.empty() || config.tlsKey.empty())) {
        log::error("--https-port needs --tls-cert and --tls-key");
        return 2;
    }
    auto service = platform::PlatformService::create(config);
    if (!service) {
        log::error("{}", service.error().message);
        return 1;
    }
    if (!resetLink.empty()) {
        auto link = (*service)->passwordResetLink(resetLink);
        if (!link) {
            log::error("{}", link.error().message);
            return 1;
        }
        std::printf("%s\n", link->c_str());
        log::info("Give this link to '{}' (it works once, for an hour)", resetLink);
        return 0;
    }
    if (!makeAdmin.empty()) {
        if (auto s = (*service)->setRole(makeAdmin, online::Role::Admin); !s) {
            log::error("{}", s.error().message);
            return 1;
        }
        log::info("'{}' is now an administrator", makeAdmin);
        return 0;
    }
    if (auto s = (*service)->start(); !s) {
        log::error("{}", s.error().message);
        return 1;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    log::info("Stopping");
    (*service)->stop();
    return 0;
}
