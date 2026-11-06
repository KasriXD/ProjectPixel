#pragma once

#include "PlatformService.hpp"

namespace httplib {
class Server;
}

namespace px::platform {

// Email addresses, password resets and changes, deleting an account and
// downloading its data (Accounts.cpp, docs/accounts.md).
void registerAccountRoutes(httplib::Server& http, PlatformService::Context& context);

} // namespace px::platform
