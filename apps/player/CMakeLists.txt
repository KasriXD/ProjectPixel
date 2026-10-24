# px-player: the desktop game client. Loads a project and plays its start scene.
add_executable(px_player main.cpp PlayerApp.cpp)
set_target_properties(px_player PROPERTIES OUTPUT_NAME px-player)
target_link_libraries(px_player PRIVATE px::runtime px::project px::net px::render px::platform_sdl)
px_target_defaults(px_player)

if(EMSCRIPTEN)
    # Browser build: the example project is packaged into px-player.data and
    # mounted at /examples/platformer in the in-memory file system.
    set_target_properties(px_player PROPERTIES SUFFIX ".html")
    target_compile_definitions(px_player PRIVATE PX_DEFAULT_PROJECT_DIR="/examples/platformer")
    target_link_options(px_player PRIVATE
        "--preload-file=${PX_EXAMPLES_DIR}/platformer@/examples/platformer"
        "--pre-js=${CMAKE_CURRENT_SOURCE_DIR}/web/pre.js")
    set_property(TARGET px_player APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/web/pre.js")
else()
    # Development convenience: with no argument, play the bundled example.
    target_compile_definitions(px_player PRIVATE PX_DEFAULT_PROJECT_DIR="${PX_EXAMPLES_DIR}/platformer")
endif()

if(WIN32)
    # Windowed application without a console in release builds.
    set_target_properties(px_player PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
endif()
