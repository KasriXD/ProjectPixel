add_executable(px_tests
    test_main.cpp
    test_core.cpp
    test_assets.cpp
    test_scene.cpp
    test_physics.cpp
    test_project.cpp
    test_runtime.cpp
    test_render.cpp)

target_link_libraries(px_tests PRIVATE
    px_doctest
    px::core px::assets px::scene px::physics px::project px::runtime px::render)
target_compile_definitions(px_tests PRIVATE PX_EXAMPLES_DIR="${PX_EXAMPLES_DIR}")
px_target_defaults(px_tests)

# One CTest entry per doctest test case would be nicer; one entry keeps CI simple.
add_test(NAME unit_tests COMMAND px_tests)

# End-to-end: play the example level headlessly with a scripted route and
# require that every coin is collected.
add_test(NAME example_platformer_playthrough
    COMMAND px_headless "${PX_EXAMPLES_DIR}/platformer"
            --input-script "${PX_EXAMPLES_DIR}/platformer/tests/full_run.input"
            --expect-all-collected)

# Smoke test for the real client with no display: SDL's "dummy" video driver
# and software renderer, 120 deterministic frames, then a screenshot.
# (The "offscreen" driver is not used: on macOS SDL makes its windows OpenGL
# windows by default, and offscreen OpenGL needs EGL, which macOS lacks.)
if(PX_BUILD_CLIENT)
    add_test(NAME player_smoke_headless_display
        COMMAND px_player "${PX_EXAMPLES_DIR}/platformer"
                --frames 120 --screenshot "${CMAKE_CURRENT_BINARY_DIR}/player_smoke.png")
    set_tests_properties(player_smoke_headless_display PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_FRAMEBUFFER_ACCELERATION=0")
endif()
