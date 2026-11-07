# px-platform: accounts, catalog, publishing, game servers, saved data and
# moderation over HTTP (docs/platform.md). The service itself is a library
# so the tests can run it in-process.
include("${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake")
set(_px_web "${CMAKE_CURRENT_SOURCE_DIR}/web")
px_embed_files(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/WebFiles.generated.cpp"
    NAMESPACE px::platform FUNCTION findWebFile HEADER WebFiles.hpp
    FILES "play.html=${_px_web}/play.html=text/html"
          "play.js=${_px_web}/play.js=text/javascript"
          "play.css=${_px_web}/play.css=text/css"
          "avatars.png=${_px_web}/avatars.png=image/png")

add_library(px_platform_service STATIC
    src/Accounts.cpp
    src/Certificates.cpp
    src/Database.cpp
    src/Fleet.cpp
    src/Leaderboards.cpp
    src/Mail.cpp
    src/Operations.cpp
    src/PlatformService.cpp
    src/Process.cpp
    src/Social.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/WebFiles.generated.cpp")
target_include_directories(px_platform_service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(px_platform_service
    PUBLIC px::core px::crypto px::online px::net
    PRIVATE px_httplib px_sqlite nlohmann_json::nlohmann_json)
px_target_defaults(px_platform_service)

add_executable(px_platform src/main.cpp)
set_target_properties(px_platform PROPERTIES OUTPUT_NAME px-platform)
target_link_libraries(px_platform PRIVATE px_platform_service)
target_compile_definitions(px_platform PRIVATE
    PX_HEADLESS_PATH="$<TARGET_FILE:px_headless>"
    PX_SERVER_PATH="$<TARGET_FILE:px_server>")
add_dependencies(px_platform px_headless px_server)
px_target_defaults(px_platform)
