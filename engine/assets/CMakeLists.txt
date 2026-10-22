px_add_module(assets
    SOURCES
        src/AssetRegistry.cpp
        src/Image.cpp
    DEPS
        px::core
    PRIVATE_DEPS
        px_stb_image)
