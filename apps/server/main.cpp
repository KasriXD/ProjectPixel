// px-server: the dedicated multiplayer server. Loads a project, runs its
// start scene as the only authoritative simulation and lets players join
// over UDP (docs/multiplayer.md). No graphics, no window.
//
//   px-server <project-dir> [--port N] [--max-players N] [--name TEXT]
//             [--snapshot-rate HZ] [--restart-delay SECONDS] [--ticks N] [--verbose]
//
// Exit codes: 0 stopped normally, 1 could not start, 2 bad arguments.

#include "px/core/FileSystem.hpp"
#include "px/core/Log.hpp"
#include "px/core/Version.hpp"
#include "px/net/GameServer.hpp"
#include "px/net/Udp.hpp"
#include "px/runtime/GameLoader.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

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

    DirectoryFileSystem files(options->projectDir);
    auto game = runtime::loadGame(files);
    if (!game) {
        log::error("{}", game.error().message);
        return 1;
    }
    for (const auto& w : game->warnings) log::warn("{}", w);

    auto endpoint = net::listenUdp(static_cast<std::uint16_t>(options->port),
                                   static_cast<std::size_t>(options->maxPlayers));
    if (!endpoint) {
        log::error("{}", endpoint.error().message);
        return 1;
    }
    net::ServerOptions serverOptions;
    serverOptions.name = options->name.empty() ? game->manifest.name : options->name;
    serverOptions.maxPlayers = static_cast<std::size_t>(options->maxPlayers);
    const int tickRate = std::max(1, game->manifest.settings.tickRate);
    serverOptions.snapshotInterval = std::max(1, tickRate / std::min(options->snapshotRate, tickRate));
    serverOptions.restartDelaySeconds = options->restartDelay;
    auto created = net::GameServer::create(game.value(), std::move(endpoint.value()), serverOptions);
    if (!created) {
        log::error("{}", created.error().message);
        return 1;
    }
    net::GameServer& server = *created.value();
    log::info("{} server {}: '{}' on UDP port {}, up to {} players, {} ticks/s, {} snapshots/s",
              version::productName, version::string, game->manifest.name, options->port, options->maxPlayers,
              tickRate, tickRate / serverOptions.snapshotInterval);
    log::info("Players join with: px-player --connect <this computer's address>:{} <project>. Ctrl+C stops.",
              options->port);

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
    server.shutdown();
    return 0;
}
