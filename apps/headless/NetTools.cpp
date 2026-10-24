#include "NetTools.hpp"

#include "px/core/Log.hpp"
#include "px/net/GameClient.hpp"
#include "px/net/GameServer.hpp"
#include "px/net/Loopback.hpp"
#include "px/net/Udp.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <memory>
#include <thread>
#include <vector>

namespace px::headless {

namespace {

const char* statusName(net::GameClient::Status s) {
    switch (s) {
        case net::GameClient::Status::Connecting: return "connecting";
        case net::GameClient::Status::Joining: return "joining";
        case net::GameClient::Status::Playing: return "playing";
        case net::GameClient::Status::Disconnected: return "disconnected";
    }
    return "?";
}

} // namespace

int runBot(const runtime::LoadedGame& game, const NetToolOptions& options) {
    auto address = net::parseAddress(options.connectAddress, net::kDefaultPort);
    if (!address) {
        log::error("{}", address.error().message);
        return 2;
    }
    auto endpoint = net::connectUdp(address->first, address->second);
    if (!endpoint) {
        log::error("{}", endpoint.error().message);
        return 1;
    }
    net::ClientOptions clientOptions;
    clientOptions.playerName = options.playerName.empty() ? std::string("Bot") : options.playerName;
    auto created = net::GameClient::create(game, std::move(endpoint.value()), clientOptions);
    if (!created) {
        log::error("{}", created.error().message);
        return 1;
    }
    net::GameClient& client = *created.value();
    std::optional<runtime::InputScriptPlayer> script;
    if (options.script) script.emplace(*options.script);
    const int rate = client.tickRate();
    const int playTicks = options.ticks.value_or(options.script ? options.script->totalTicks() + rate : 10 * rate);

    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    const auto tickLength = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate));
    auto next = start;
    int played = 0;
    bool joined = false;
    bool completed = false; // the server restarts the level some seconds after it is complete
    while (played < playTicks && client.status() != net::GameClient::Status::Disconnected) {
        const double now = std::chrono::duration<double>(Clock::now() - start).count();
        client.receive(now);
        completed = completed || client.game().levelComplete;
        if (client.status() == net::GameClient::Status::Playing) {
            joined = true;
            client.tick(script ? script->next() : runtime::InputFrame{});
            ++played;
        }
        client.updateView(now);
        next += tickLength;
        std::this_thread::sleep_until(next);
    }
    const auto stats = client.stats();
    const auto pos = client.predictedPosition();
    std::printf("bot: %s%s%s, played %d ticks, score=%d levelCompleted=%s players=%zu snapshots=%llu "
                "corrections=%llu rtt=%.0fms position=%s\n",
                statusName(client.status()), client.status() == net::GameClient::Status::Disconnected ? ": " : "",
                client.status() == net::GameClient::Status::Disconnected ? client.statusText().c_str() : "", played,
                client.game().score, completed ? "true" : "false", client.game().players.size(),
                static_cast<unsigned long long>(stats.snapshots), static_cast<unsigned long long>(stats.corrections),
                stats.roundTripMs, pos ? std::format("({:.2f}, {:.2f})", pos->x, pos->y).c_str() : "none");
    client.disconnect();
    if (options.expectJoined && !joined) {
        log::error("Expected to join the server");
        return 3;
    }
    if (options.expectLevelComplete && !completed) {
        log::error("Expected the level to be completed");
        return 3;
    }
    return 0;
}

int runNetTest(const runtime::LoadedGame& game, const NetToolOptions& options) {
    const auto port = static_cast<std::uint16_t>(options.port > 0 ? options.port : net::kDefaultPort);
    net::LoopbackNetwork loopback(1);
    net::LinkConditions link;
    link.latencyMs = options.latencyMs;
    link.jitterMs = options.jitterMs;
    link.lossPercent = options.lossPercent;

    auto listening = options.udp ? net::listenUdp(port, static_cast<std::size_t>(options.clients))
                                 : loopback.listen(port, static_cast<std::size_t>(options.clients));
    if (!listening) {
        log::error("{}", listening.error().message);
        return 1;
    }
    net::ServerOptions serverOptions;
    serverOptions.maxPlayers = static_cast<std::size_t>(options.clients);
    serverOptions.restartDelaySeconds = -1.0; // keep the result for the report
    auto createdServer = net::GameServer::create(game, std::move(listening.value()), serverOptions);
    if (!createdServer) {
        log::error("{}", createdServer.error().message);
        return 1;
    }
    net::GameServer& server = *createdServer.value();

    std::vector<std::unique_ptr<net::GameClient>> clients;
    for (int i = 0; i < options.clients; ++i) {
        auto endpoint = options.udp ? net::connectUdp("127.0.0.1", port)
                                    : Result<std::unique_ptr<net::INetEndpoint>>(loopback.connect(port, link));
        if (!endpoint) {
            log::error("{}", endpoint.error().message);
            return 1;
        }
        net::ClientOptions clientOptions;
        clientOptions.playerName = std::format("Bot {}", i + 1);
        auto client = net::GameClient::create(game, std::move(endpoint.value()), clientOptions);
        if (!client) {
            log::error("{}", client.error().message);
            return 1;
        }
        clients.push_back(std::move(client.value()));
    }

    std::optional<runtime::InputScriptPlayer> script;
    if (options.script) script.emplace(*options.script);
    const int rate = server.tickRate();
    const int ticks = options.ticks.value_or(options.script ? options.script->totalTicks() + 3 * rate : 10 * rate);
    std::printf("net-test: %d clients over %s (latency %.0f ms, jitter %.0f ms, loss %.1f %%), %d ticks\n",
                options.clients, options.udp ? "UDP on 127.0.0.1" : "a simulated network", options.latencyMs,
                options.jitterMs, options.lossPercent, ticks);

    double time = 0.0;
    std::vector<bool> joined(clients.size(), false);
    for (int t = 0; t < ticks; ++t) {
        time += 1.0 / rate;
        loopback.setTime(time);
        server.tick();
        for (std::size_t i = 0; i < clients.size(); ++i) {
            net::GameClient& c = *clients[i];
            c.receive(time);
            runtime::InputFrame frame;
            if (c.status() == net::GameClient::Status::Playing) {
                joined[i] = true;
                if (i == 0 && script) frame = script->next();
            }
            c.tick(frame);
            c.updateView(time);
        }
        if (options.udp) std::this_thread::sleep_for(std::chrono::microseconds(200)); // let the OS deliver
    }

    const auto& state = server.session().state();
    std::printf("server: ticks=%llu score=%d levelComplete=%s scriptErrors=%d players=%zu lastSnapshot=%zuB\n",
                static_cast<unsigned long long>(server.ticksRun()), state.score, state.levelComplete ? "true" : "false",
                state.scriptErrors, server.playerCount(), server.lastSnapshotBytes());
    const double seconds = static_cast<double>(ticks) / rate;
    bool allJoined = true;
    for (std::size_t i = 0; i < clients.size(); ++i) {
        net::GameClient& c = *clients[i];
        const auto stats = c.stats();
        std::string error = "-";
        if (const auto predicted = c.predictedPosition()) {
            const Vec2 truth = server.session().scene().worldPosition(server.session().characterOf(c.player()));
            error = std::format("{:.3f}", std::hypot(predicted->x - truth.x, predicted->y - truth.y));
        }
        std::printf("client %zu: %s, snapshots %llu (dropped %llu), corrections %llu (last %.2f), down %.1f kB/s, "
                    "up %.1f kB/s, own position error %s\n",
                    i + 1, statusName(c.status()), static_cast<unsigned long long>(stats.snapshots),
                    static_cast<unsigned long long>(stats.snapshotsDropped),
                    static_cast<unsigned long long>(stats.corrections), stats.lastCorrection,
                    static_cast<double>(stats.bytesReceived) / 1024.0 / seconds,
                    static_cast<double>(stats.bytesSent) / 1024.0 / seconds, error.c_str());
        allJoined = allJoined && joined[i];
    }
    for (auto& c : clients) c->disconnect();
    server.shutdown();

    if (options.expectJoined && !allJoined) {
        log::error("Expected every client to join");
        return 3;
    }
    if (options.expectLevelComplete && !state.levelComplete) {
        log::error("Expected the level to be completed");
        return 3;
    }
    if (options.expectNoScriptErrors && state.scriptErrors > 0) {
        log::error("Expected no script errors, got {}: {}", state.scriptErrors, state.lastScriptError);
        return 3;
    }
    return 0;
}

} // namespace px::headless
