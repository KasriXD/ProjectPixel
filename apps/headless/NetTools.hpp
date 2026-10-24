#pragma once

#include "px/runtime/GameLoader.hpp"
#include "px/runtime/InputScript.hpp"

#include <optional>
#include <string>

// Multiplayer tools of px-headless: a scripted bot that joins a real server,
// and a self-contained test that runs a server and several clients in one
// process over a simulated (or real UDP) network and reports how they did.
namespace px::headless {

struct NetToolOptions {
    // --connect: join this server as a bot.
    std::string connectAddress;
    std::string playerName;
    // --net-test
    int clients = 2;
    double latencyMs = 0.0;
    double jitterMs = 0.0;
    double lossPercent = 0.0;
    bool udp = false;
    int port = 0; // 0: the default port
    // shared
    std::optional<runtime::InputScript> script; // played by the bot / by client 1
    std::optional<int> ticks;
    bool expectLevelComplete = false;
    bool expectNoScriptErrors = false;
    bool expectJoined = false;
};

// Exit codes as px-headless: 0 fine, 1 error, 3 an expectation failed.
int runBot(const runtime::LoadedGame& game, const NetToolOptions& options);
int runNetTest(const runtime::LoadedGame& game, const NetToolOptions& options);

} // namespace px::headless
