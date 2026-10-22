# px-player: the desktop game client. Loads a project and plays its start scene.
add_executable(px_player main.cpp PlayerApp.cpp)
set_target_properties(px_player PROPERTIES OUTPUT_NAME px-player)
target_link_libraries(px_player PRIVATE px::runtime px::project px::render px::platform_sdl)
# Development convenience: with no argument, play the bundled example.
target_compile_definitions(px_player PRIVATE PX_DEFAULT_PROJECT_DIR="${PX_EXAMPLES_DIR}/platformer")
px_target_defaults(px_player)

if(WIN32)
    # Windowed application without a console in release builds.
    set_target_properties(px_player PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
endif()
