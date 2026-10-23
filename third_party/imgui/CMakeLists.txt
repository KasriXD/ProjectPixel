# Dear ImGui (MIT), docking branch: Studio's user interface. Vendored by
# tools/update_imgui.py; version in VERSION and third_party/VERSIONS.md.
# Built only together with Studio; px-player and px-headless never link it.
add_library(px_imgui STATIC
    imgui.cpp
    imgui_demo.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    backends/imgui_impl_sdl3.cpp
    backends/imgui_impl_sdlrenderer3.cpp
    misc/cpp/imgui_stdlib.cpp)
target_include_directories(px_imgui SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/backends"
    "${CMAKE_CURRENT_SOURCE_DIR}/misc/cpp")
target_compile_definitions(px_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_link_libraries(px_imgui PUBLIC SDL3::SDL3)
target_compile_features(px_imgui PUBLIC cxx_std_20)
if(MSVC)
    target_compile_options(px_imgui PRIVATE /W0)
else()
    target_compile_options(px_imgui PRIVATE -w)
endif()
if(PX_ENABLE_SANITIZERS AND NOT MSVC)
    target_compile_options(px_imgui PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
