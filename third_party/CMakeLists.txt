# Vendored single-header libraries. Versions and sources: third_party/VERSIONS.md

# stb_image: compiled once in its own translation unit, without project warnings.
add_library(px_stb_image STATIC stb/stb_image_impl.c)
target_include_directories(px_stb_image SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/stb")
set_target_properties(px_stb_image PROPERTIES POSITION_INDEPENDENT_CODE ON)

# doctest: header-only test framework.
add_library(px_doctest INTERFACE)
target_include_directories(px_doctest SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/doctest")
