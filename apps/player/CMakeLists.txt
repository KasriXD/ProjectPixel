# px-player: the game client. Loads a project or a game package and plays
# its start scene (desktop, browser, Android, iOS).
if(ANDROID)
    # SDL's Java activity loads the game as libmain.so (platforms/android).
    add_library(px_player SHARED main.cpp PlayerApp.cpp PlayerSettings.cpp)
    set_target_properties(px_player PROPERTIES OUTPUT_NAME main)
else()
    add_executable(px_player main.cpp PlayerApp.cpp PlayerSettings.cpp)
    set_target_properties(px_player PROPERTIES OUTPUT_NAME px-player)
endif()
target_link_libraries(px_player PRIVATE px::runtime px::project px::net px::render px::platform_sdl)
px_target_defaults(px_player)

if(EMSCRIPTEN)
    # Browser build: the example project is packaged into px-player.data and
    # mounted at /examples/platformer in the in-memory file system (with the
    # top-down example's routes, for the browser cross-play test). Platform
    # games arrive as packages (web/pre.js).
    set_target_properties(px_player PROPERTIES SUFFIX ".html")
    target_compile_definitions(px_player PRIVATE PX_DEFAULT_PROJECT_DIR="/examples/platformer")
    target_link_options(px_player PRIVATE
        "--preload-file=${PX_EXAMPLES_DIR}/platformer@/examples/platformer"
        "--preload-file=${PX_EXAMPLES_DIR}/topdown/tests@/examples/topdown/tests"
        "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/web/pre.js")
    set_property(TARGET px_player APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/pre.js")
else()
    # Development convenience: with no argument, play the bundled example.
    target_compile_definitions(px_player PRIVATE PX_DEFAULT_PROJECT_DIR="${PX_EXAMPLES_DIR}/platformer")
endif()

# Phones and tablets play the game that comes in the app: game.pxpkg in the
# APK's assets (platforms/android) or the iOS bundle (PX_GAME_PACKAGE).
if(ANDROID OR IOS)
    target_compile_definitions(px_player PRIVATE PX_MOBILE_PACKAGE="game.pxpkg")
endif()
if(IOS)
    set(PX_GAME_PACKAGE "" CACHE FILEPATH "Game package (*.pxpkg) the iOS app plays (px-headless <project> --pack)")
    if(NOT EXISTS "${PX_GAME_PACKAGE}")
        message(FATAL_ERROR "The iOS app needs a game: -DPX_GAME_PACKAGE=/path/to/game.pxpkg "
                            "(make one with: px-headless examples/platformer --pack game.pxpkg)")
    endif()
    configure_file("${PX_GAME_PACKAGE}" "${CMAKE_CURRENT_BINARY_DIR}/game.pxpkg" COPYONLY)
    set(_px_game "${CMAKE_CURRENT_BINARY_DIR}/game.pxpkg")
    # HTTPS on iOS: the launcher trusts the certificates in this PEM file
    # (iOS gives Mbed TLS no access to the system's; docs/launcher.md).
    set(PX_CA_BUNDLE "" CACHE FILEPATH "PEM file of trusted root certificates for HTTPS platforms (iOS)")
    if(PX_CA_BUNDLE)
        configure_file("${PX_CA_BUNDLE}" "${CMAKE_CURRENT_BINARY_DIR}/ca-certificates.pem" COPYONLY)
        list(APPEND _px_game "${CMAKE_CURRENT_BINARY_DIR}/ca-certificates.pem")
    endif()
    target_sources(px_player PRIVATE ${_px_game})
    set_target_properties(px_player PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER dev.px.player
        MACOSX_BUNDLE_BUNDLE_NAME "Pixel Player"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        RESOURCE "${_px_game}"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER dev.px.player
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO)
endif()

if(WIN32)
    # Windowed application without a console in release builds.
    set_target_properties(px_player PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
endif()

if(PX_ENABLE_PLATFORM)
    target_link_libraries(px_player PRIVATE px::online)
    target_compile_definitions(px_player PRIVATE PX_HAVE_PLATFORM=1)
endif()

# The launcher (docs/launcher.md): Dear ImGui, with the account portraits
# compiled in (the same picture the platform's web page uses).
if(PX_LAUNCHER_ENABLED)
    include("${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake")
    px_embed_files(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/LauncherFiles.generated.cpp"
        NAMESPACE px::player FUNCTION findLauncherFile HEADER LauncherFiles.hpp
        FILES "avatars.png=${PROJECT_SOURCE_DIR}/services/platform/web/avatars.png=image/png")
    target_sources(px_player PRIVATE Launcher.cpp "${CMAKE_CURRENT_BINARY_DIR}/LauncherFiles.generated.cpp")
    target_include_directories(px_player PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(px_player PRIVATE px_imgui)
    target_compile_definitions(px_player PRIVATE PX_HAVE_LAUNCHER=1)
endif()
