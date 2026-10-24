# px-studio: the desktop editor (Dear ImGui on SDL3). The editor model lives
# in engine/editor (px::editor) and is tested without UI; this folder is the UI.
add_executable(px_studio
    main.cpp
    StudioApp.cpp
    StudioSettings.cpp
    FileDialogs.cpp
    UiHelpers.cpp
    HierarchyPanel.cpp
    InspectorPanel.cpp
    ViewportPanel.cpp
    AssetsPanel.cpp
    ConsolePanel.cpp
    ScriptEditorPanel.cpp
    ProjectSettingsPanel.cpp
    ToolboxPanel.cpp
    MultiplayerTest.cpp)
set_target_properties(px_studio PROPERTIES OUTPUT_NAME px-studio)
target_link_libraries(px_studio PRIVATE px::editor px::render px::platform_sdl px_imgui)
px_target_defaults(px_studio)
target_compile_definitions(px_studio PRIVATE
    PX_EXAMPLES_DIR="${PX_EXAMPLES_DIR}"
    PX_LIBRARY_DIR="${CMAKE_SOURCE_DIR}/library"
    PX_DOCS_DIR="${CMAKE_SOURCE_DIR}/docs"
    # Where the build puts the programs Studio starts for multiplayer tests
    # (a package keeps them next to px-studio instead).
    PX_SERVER_PATH="$<TARGET_FILE:px_server>"
    PX_PLAYER_PATH="$<TARGET_FILE:px_player>")
add_dependencies(px_studio px_server px_player)

if(WIN32)
    # Windowed application without a console in release builds.
    set_target_properties(px_studio PROPERTIES WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>)
endif()
