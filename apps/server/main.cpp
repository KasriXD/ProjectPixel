// px-server: the dedicated multiplayer server. Loads a project, runs its
// start scene as the only authoritative simulation and lets players join
// over UDP (docs/multiplayer.md). No graphics, no window.
//
//   px-server <project-dir or FILE.pxpkg> [--port N] [--max-players N] [--name TEXT]
//             [--snapshot-rate HZ] [--restart-delay SECONDS] [--ticks N] [--verbose]
//             [--platform URL --session ID --platform-key HEX] [--empty-timeout SECONDS]
//             [--websocket-port N | --no-websocket] [--tls-cert FILE --tls-key FILE]
//
// Players join over UDP (desktop) or WebSockets (browsers) on the same port
// number (TCP for WebSockets), into the same game.
//
// With --platform the server was started by the platform (docs/platform.md):
// players need connect tokens, their saved data is loaded and saved through
// the platform (the server key comes in the PX_SERVER_KEY environment
// variable), and the server stops by itself once nobody is playing.
//
// Exit codes: 0 stopped normally, 1 could not start, 2 bad arguments.

#include "px/core/FileSystem.hpp"
#include "px/core/Log.hpp"
#include "px/core/Version.hpp"
#include "px/crypto/Encoding.hpp"
#include "px/net/GameServer.hpp"
#include "px/net/Udp.hpp"
#include "px/net/WebSocket.hpp"
#include "px/project/Package.hpp"
#include "px/runtime/GameLoader.hpp"
#if defined(PX_HAVE_PLATFORM)
#include "px/online/PlatformClient.hpp"
#include "px/online/PlatformPlayerStore.hpp"
#endif

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

using namespace px;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) { g_stop = true; }

struct Options {
    std::string projectDir;
    int port = net::kDefaultPort;
    int maxPlayers = 8;
    std::string name;
    int snapshotRate = 30;
    double restartDelay = 8.0;
    long long ticks = 0; // 0: run until stopped
    bool verbose = false;
    // Started by the platform.
    std::string platformUrl;
    std::string sessionId;
    std::string platformKey; // hex
    double emptyTimeout = -1.0; // seconds without players before stopping (platform default 30)
    int webSocketPort = -1;     // -1: the same number as the UDP port; 0: none
    std::string tlsCert;        // wss:// for browsers on https:// pages
    std::string tlsKey;
};

void printUsage() {
    std::fprintf(stderr,
                 "%s dedicated server %s\n"
                 "usage: px-server <project-dir> [--port N] [--max-players N] [--name TEXT]\n"
                 "                 [--snapshot-rate HZ] [--restart-delay SECONDS] [--ticks N] [--verbose]\n"
                 "\n"
                 "  --port N             UDP port to listen on (default %d)\n"
                 "  --max-players N      players allowed at once, 1-64 (default 8)\n"
                 "  --name TEXT          server name shown to players\n"
                 "  --snapshot-rate HZ   world updates sent per second (default 30)\n"
                 "  --restart-delay S    seconds after the level is complete before it restarts\n"
                 "                       (default 8; -1: never)\n"
                 "  --ticks N            stop after N simulation ticks (for tests)\n"
                 "  --platform URL       started by the platform at URL (with --session and\n"
                 "                       --platform-key; the server key is in PX_SERVER_KEY)\n"
                 "  --empty-timeout S    stop after S seconds without players\n"
                 "  --websocket-port N   TCP port for browsers (WebSockets; default: the UDP port number)\n"
                 "  --no-websocket       UDP only\n"
                 "  --tls-cert FILE      with --tls-key: secure WebSockets (wss://), for https:// pages\n"
                 "\n"
                 "Players join with: px-player --connect <address>[:port] [project-dir]\n"
                 "Stop the server with Ctrl+C.\n",
                 version::productName, version::string, net::kDefaultPort);
}

template <class T>
bool parseNumber(const char* v, T& out) {
    if (v == nullptr) return false;
    const std::string_view s(v);
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && ptr == s.data() + s.size();
}

std::optional<Options> parseArgs(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--port") {
            if (!parseNumber(value(), o.port) || o.port < 1 || o.port > 65535) return std::nullopt;
        } else if (arg == "--max-players") {
            if (!parseNumber(value(), o.maxPlayers) || o.maxPlayers < 1 || o.maxPlayers > 64) return std::nullopt;
        } else if (arg == "--name") {
            const char* v = value();
            if (v == nullptr) return std::nullopt;
            o.name = v;
        } else if (arg == "--snapshot-rate") {
            if (!parseNumber(value(), o.snapshotRate) || o.snapshotRate < 1 || o.snapshotRate > 240) return std::nullopt;
        } else if (arg == "--restart-delay") {
            const char* v = value();
            if (v == nullptr) return std::nullopt;
            try {
                o.restartDelay = std::stod(v);
            } catch (...) {
                return std::nullopt;
            }
        } else if (arg == "--ticks") {
            if (!parseNumber(value(), o.ticks) || o.ticks < 1) return std::nullopt;
        } else if (arg == "--verbose") {
            o.verbose = true;
        } else if (arg == "--platform" || arg == "--session" || arg == "--platform-key") {
            const char* v = value();
            if (v == nullptr) return std::nullopt;
            (arg == "--platform" ? o.platformUrl : arg == "--session" ? o.sessionId : o.platformKey) = v;
        } else if (arg == "--websocket-port") {
            if (!parseNumber(value(), o.webSocketPort) || o.webSocketPort < 1 || o.webSocketPort > 65535) return std::nullopt;
        } else if (arg == "--no-websocket") {
            o.webSocketPort = 0;
        } else if (arg == "--tls-cert" || arg == "--tls-key") {
            const char* v = value();
            if (v == nullptr) return std::nullopt;
            (arg == "--tls-cert" ? o.tlsCert : o.tlsKey) = v;
        } else if (arg == "--empty-timeout") {
            const char* v = value();
            if (v == nullptr) return std::nullopt;
            try {
                o.emptyTimeout = std::stod(v);
            } catch (...) {
                return std::nullopt;
            }
        } else if (arg == "--help" || arg == "-h") {
            return std::nullopt;
        } else if (!arg.starts_with("--") && o.projectDir.empty()) {
            o.projectDir = arg;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return std::nullopt;
        }
    }
    if (o.projectDir.empty()) return std::nullopt;
    if (!o.platformUrl.empty() && (o.sessionId.empty() || o.platformKey.empty())) return std::nullopt;
    return o;
}

} // namespace

int main(int argc, char** argv) {
    const auto options = parseArgs(argc, argv);
    if (!options) {
        printUsage();
        return 2;
    }
    if (options->verbose) log::setMinLevel(log::Level::Debug);

    std::unique_ptr<IFileSystem> files;
    if (options->projectDir.ends_with(".pxpkg")) {
        auto package = project::readPackageFile(options->projectDir);
        if (!package) {
            log::error("{}", package.error().message);
            return 1;
        }
        files = package->toFileSystem();
    } else {
        files = std::make_unique<DirectoryFileSystem>(options->projectDir);
    }
    auto game = runtime::loadGame(*files);
    if (!game) {
        log::error("{}", game.error().message);
        return 1;
    }
    for (const auto& w : game->warnings) log::warn("{}", w);

    std::vector<std::unique_ptr<net::INetEndpoint>> endpoints;
    if (net::udpSupported()) {
        auto udp = net::listenUdp(static_cast<std::uint16_t>(options->port),
                                  static_cast<std::size_t>(options->maxPlayers));
        if (!udp) {
            log::error("{}", udp.error().message);
            return 1;
        }
        endpoints.push_back(std::move(udp.value()));
    }
    const int wsPort = options->webSocketPort < 0 ? options->port : options->webSocketPort;
    if (wsPort > 0 && net::webSocketServerSupported()) {
        net::WebSocketServerOptions ws;
        ws.port = static_cast<std::uint16_t>(wsPort);
        ws.maxPeers = static_cast<std::size_t>(options->maxPlayers) + 4; // room to refuse politely
        ws.tlsCert = options->tlsCert;
        ws.tlsKey = options->tlsKey;
        auto listening = net::listenWebSocket(ws);
        if (!listening) {
            log::error("{}", listening.error().message);
            return 1;
        }
        endpoints.push_back(std::move(listening.value()));
        log::info("Browsers join with {}://<this computer's address>:{}/play", ws.tlsCert.empty() ? "ws" : "wss", wsPort);
    }
    if (endpoints.empty()) {
        log::error("This build can listen neither for UDP nor for WebSockets");
        return 1;
    }
    auto endpoint = Result<std::unique_ptr<net::INetEndpoint>>(net::combineEndpoints(std::move(endpoints)));
    net::ServerOptions serverOptions;
    serverOptions.name = options->name.empty() ? game->manifest.name : options->name;
    serverOptions.maxPlayers = static_cast<std::size_t>(options->maxPlayers);
    const int tickRate = std::max(1, game->manifest.settings.tickRate);
    serverOptions.snapshotInterval = std::max(1, tickRate / std::min(options->snapshotRate, tickRate));
    serverOptions.restartDelaySeconds = options->restartDelay;

    // Platform mode: tokens, saved data, heartbeats.
    const bool managed = !options->platformUrl.empty();
#if defined(PX_HAVE_PLATFORM)
    std::unique_ptr<online::PlatformClient> platform;
    if (managed) {
        const auto key = crypto::fromHex(options->platformKey);
        const char* serverKey = std::getenv("PX_SERVER_KEY");
        if (!key || key->size() != 32 || serverKey == nullptr || *serverKey == 0) {
            log::error("--platform needs --platform-key (64 hex digits) and the PX_SERVER_KEY environment variable");
            return 2;
        }
        crypto::PublicKey publicKey{};
        std::copy(key->begin(), key->end(), publicKey.begin());
        serverOptions.platformKey = publicKey;
        serverOptions.sessionId = options->sessionId;
        platform = std::make_unique<online::PlatformClient>(options->platformUrl);
        platform->setServerKey(serverKey);
        platform->setTimeoutSeconds(10);
        serverOptions.playerStore = std::make_shared<online::PlatformPlayerStore>(*platform);
    }
#else
    if (managed) {
        log::error("This build has no platform support");
        return 2;
    }
#endif
    auto created = net::GameServer::create(game.value(), std::move(endpoint.value()), serverOptions);
    if (!created) {
        log::error("{}", created.error().message);
        return 1;
    }
    net::GameServer& server = *created.value();
    log::info("{} server {}: '{}' on UDP port {}, up to {} players, {} ticks/s, {} snapshots/s",
              version::productName, version::string, game->manifest.name, options->port, options->maxPlayers,
              tickRate, tickRate / serverOptions.snapshotInterval);
    if (!managed) {
        log::info("Players join with: px-player --connect <this computer's address>:{} <project>. Ctrl+C stops.",
                  options->port);
    }

#if defined(PX_HAVE_PLATFORM)
    // Tell the platform the server is ready, then report who is playing every
    // few seconds on a separate thread (the simulation never waits for HTTP).
    std::mutex playersMutex;
    std::vector<std::string> playing;
    std::atomic<bool> linkStop{false};
    std::thread link;
    if (managed) {
        Status ready = makeError(ErrorCode::IoError, "not tried");
        for (int attempt = 0; attempt < 5 && !ready; ++attempt) {
            ready = platform->serverReady(static_cast<std::uint16_t>(options->port));
            if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
        if (!ready) {
            log::error("Cannot reach the platform: {}", ready.error().message);
            return 1;
        }
        link = std::thread([&] {
            online::PlatformClient beat(*platform);
            int failures = 0;
            while (!linkStop && !g_stop) {
                std::vector<std::string> ids;
                {
                    std::lock_guard lock(playersMutex);
                    ids = playing;
                }
                auto stop = beat.serverHeartbeat(ids);
                if (stop && stop.value()) {
                    log::info("The platform asked this server to stop");
                    g_stop = true;
                } else if (!stop) {
                    // A platform that stays unreachable means nobody can save; stop cleanly.
                    if (++failures >= 6) {
                        log::error("Lost the platform: {}", stop.error().message);
                        g_stop = true;
                    }
                } else {
                    failures = 0;
                }
                for (int i = 0; i < 25 && !linkStop && !g_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        });
    }
#endif
    const double emptyTimeout = options->emptyTimeout >= 0.0 ? options->emptyTimeout : (managed ? 30.0 : -1.0);
    long long emptyTicks = 0;
    bool hadPlayers = false;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifdef _WIN32
    timeBeginPeriod(1); // 1 ms sleep resolution instead of ~16 ms
#endif

    using Clock = std::chrono::steady_clock;
    const auto tickLength = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / tickRate));
    auto next = Clock::now();
    auto lastReport = Clock::now();
    long long ticks = 0;
    while (!g_stop) {
        server.tick();
        ++ticks;
        if (options->ticks > 0 && ticks >= options->ticks) break;
        next += tickLength;
        const auto now = Clock::now();
        if (now < next) {
            std::this_thread::sleep_until(next);
        } else if (now - next > std::chrono::seconds(1)) {
            log::warn("The server fell {:.1f} s behind; skipping ahead",
                      std::chrono::duration<double>(now - next).count());
            next = now;
        }
        if (emptyTimeout >= 0.0) {
            if (server.playerCount() > 0) {
                hadPlayers = true;
                emptyTicks = 0;
            } else {
                ++emptyTicks;
                // Before anyone arrives the first player gets twice as long.
                const double limit = hadPlayers ? emptyTimeout : 2.0 * emptyTimeout;
                if (static_cast<double>(emptyTicks) / tickRate > limit) {
                    log::info("Nobody is playing; stopping");
                    break;
                }
            }
        }
#if defined(PX_HAVE_PLATFORM)
        if (managed && ticks % tickRate == 0) {
            std::lock_guard lock(playersMutex);
            playing.clear();
            for (const auto& c : server.clients()) {
                if (!c.userId.empty()) playing.push_back(c.userId);
            }
        }
#endif
        if (Clock::now() - lastReport > std::chrono::minutes(1)) {
            lastReport = Clock::now();
            log::info("{} playing, tick {}, largest snapshot {} bytes", server.playerCount(), server.ticksRun(),
                      server.lastSnapshotBytes());
        }
    }
#ifdef _WIN32
    timeEndPeriod(1);
#endif
    log::info("Stopping after {} ticks", ticks);
    server.shutdown(); // saves everyone's data first
#if defined(PX_HAVE_PLATFORM)
    if (managed) {
        linkStop = true;
        if (link.joinable()) link.join();
        serverOptions.playerStore.reset();
        (void)platform->serverEnded();
    }
#endif
    return 0;
}
