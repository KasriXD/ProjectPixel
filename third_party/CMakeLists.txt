# Vendored single-header libraries. Versions and sources: third_party/VERSIONS.md

# stb_image: compiled once in its own translation unit, without project warnings.
add_library(px_stb_image STATIC stb/stb_image_impl.c)
target_include_directories(px_stb_image SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/stb")
set_target_properties(px_stb_image PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Luau: scripting language compiler and VM.
add_subdirectory(luau)

# ENet: UDP transport for multiplayer (not for the browser).
if(PX_ENABLE_ENET)
    add_subdirectory(enet)
endif()

# doctest: header-only test framework.
add_library(px_doctest INTERFACE)
target_include_directories(px_doctest SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/doctest")

# Dear ImGui: Studio's user interface (desktop only).
if(PX_STUDIO_ENABLED)
    add_subdirectory(imgui)
endif()
