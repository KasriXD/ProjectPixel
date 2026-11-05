#pragma once

#include "PlatformService.hpp"

namespace httplib {
class Server;
}

namespace px::platform {

// Leaderboards: scores from game servers, lists for players (Leaderboards.cpp, docs/leaderboards.md).
void registerLeaderboardRoutes(httplib::Server& http, PlatformService::Context& context);

} // namespace px::platform
