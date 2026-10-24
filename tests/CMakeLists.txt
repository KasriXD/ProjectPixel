add_executable(px_tests
    test_main.cpp
    test_core.cpp
    test_assets.cpp
    test_scene.cpp
    test_physics.cpp
    test_project.cpp
    test_runtime.cpp
    test_render.cpp
    test_scripting.cpp
    test_editor.cpp
    test_multiplayer.cpp
    test_net.cpp)

target_link_libraries(px_tests PRIVATE
    px_doctest
    px::core px::assets px::scene px::physics px::project px::scripting px::runtime px::net px::render px::editor)
target_compile_definitions(px_tests PRIVATE
    PX_EXAMPLES_DIR="${PX_EXAMPLES_DIR}"
    PX_DOCS_DIR="${CMAKE_SOURCE_DIR}/docs"
    PX_LIBRARY_DIR="${CMAKE_SOURCE_DIR}/library")
px_target_defaults(px_tests)
if(EMSCRIPTEN)
    target_link_options(px_tests PRIVATE -sNODERAWFS=1 -sEXIT_RUNTIME=1)
endif()

# One CTest entry per doctest test case would be nicer; one entry keeps CI simple.
add_test(NAME unit_tests COMMAND px_tests)

# End-to-end: play the example level headlessly with a scripted route.
# Coins, the moving platform and the win condition are Luau scripts; the
# level must be completed without any script errors.
add_test(NAME example_platformer_playthrough
    COMMAND px_headless "${PX_EXAMPLES_DIR}/platformer"
            --input-script "${PX_EXAMPLES_DIR}/platformer/tests/full_run.input"
            --expect-level-complete --expect-no-script-errors)

# The same for the top-down example: key, gate, gems, stairs.
add_test(NAME example_topdown_playthrough
    COMMAND px_headless "${PX_EXAMPLES_DIR}/topdown"
            --input-script "${PX_EXAMPLES_DIR}/topdown/tests/full_run.input"
            --expect-level-complete --expect-no-script-errors)

# Smoke test for the real client with no display: SDL's "dummy" video driver
# and software renderer, 120 deterministic frames, then a screenshot.
# (The "offscreen" driver is not used: on macOS SDL makes its windows OpenGL
# windows by default, and offscreen OpenGL needs EGL, which macOS lacks.)
if(PX_BUILD_CLIENT AND NOT EMSCRIPTEN)
    add_test(NAME player_smoke_headless_display
        COMMAND px_player "${PX_EXAMPLES_DIR}/platformer"
                --frames 120 --screenshot "${CMAKE_CURRENT_BINARY_DIR}/player_smoke.png")
    set_tests_properties(player_smoke_headless_display PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_FRAMEBUFFER_ACCELERATION=0")
endif()

# Studio smoke test: the real editor UI with SDL's "dummy" video driver. It
# opens the example project, selects and creates entities, opens the script
# editor and project settings, undoes everything, plays for two seconds and
# checks that a coin was collected; nothing is saved. See docs/studio.md.
if(PX_STUDIO_ENABLED)
    add_test(NAME studio_smoke_headless_display
        COMMAND px_studio "${PX_EXAMPLES_DIR}/platformer" --smoke-test
                --screenshot "${CMAKE_CURRENT_BINARY_DIR}/studio_smoke.png")
    set_tests_properties(studio_smoke_headless_display PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_RENDER_DRIVER=software;SDL_FRAMEBUFFER_ACCELERATION=0"
        TIMEOUT 300)
endif()
