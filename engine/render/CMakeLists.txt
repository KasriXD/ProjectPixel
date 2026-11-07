# Backend-independent rendering: renderer interface, camera, text, and turning
# a scene into draw calls. Concrete backends (SDL today) live in platform_* modules.
include("${PROJECT_SOURCE_DIR}/cmake/EmbedFiles.cmake")
set(_px_fonts "${CMAKE_CURRENT_SOURCE_DIR}/fonts")
px_embed_files(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/BuiltInFontFiles.generated.cpp"
    NAMESPACE px::render::embedded FUNCTION findBuiltInFontFile HEADER BuiltInFontFiles.hpp
    FILES "pixel-default.png=${_px_fonts}/pixel-default.png=image/png"
          "pixel-default.font.json=${_px_fonts}/pixel-default.font.json=application/json")

px_add_module(render
    SOURCES
        src/Camera2D.cpp
        src/SceneRenderer.cpp
        src/Text.cpp
        src/TextureCache.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/BuiltInFontFiles.generated.cpp"
    DEPS
        px::core
        px::assets
        px::scene
        px::project)
target_include_directories(px_render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
