# The platform from the engine's side: checking game packages before the
# platform accepts them, the platform's wire types and the HTTP client that
# Studio, the player, game servers and tools use. Desktop only (no browser
# sockets yet).
px_add_module(online
    SOURCES
        src/PackageCheck.cpp
        src/PlatformClient.cpp
        src/PlatformLeaderboardStore.cpp
        src/PlatformPlayerStore.cpp
        src/Thumbnail.cpp
        src/WireJson.cpp
    DEPS
        px::core
        px::crypto
        px::project
        px::runtime
        px::net
        px::audio
        nlohmann_json::nlohmann_json # WireJson.hpp: shared with the platform service
    PRIVATE_DEPS
        px_stb_image_write
        px_httplib)
