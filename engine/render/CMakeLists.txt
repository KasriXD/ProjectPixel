# Backend-independent rendering: renderer interface, camera, and turning a
# scene into draw calls. Concrete backends (SDL today) live in platform_* modules.
px_add_module(render
    SOURCES
        src/Camera2D.cpp
        src/SceneRenderer.cpp
        src/TextureCache.cpp
    DEPS
        px::core
        px::assets
        px::scene)
