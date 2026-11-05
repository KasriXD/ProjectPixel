#pragma once

#include "PlatformService.hpp"

namespace httplib {
class Server;
}

namespace px::platform {

// Profiles, friends, blocking, presence, joining friends and invitations
// (Social.cpp, docs/social.md).
void registerSocialRoutes(httplib::Server& http, PlatformService::Context& context);

} // namespace px::platform
