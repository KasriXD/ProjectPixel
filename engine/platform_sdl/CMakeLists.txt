# SDL3 implementation of windowing, input, sound and the 2D renderer.
# This is the only engine module that includes SDL headers.
px_add_module(platform_sdl
    SOURCES
        src/SdlAudio.cpp
        src/SdlKeys.cpp
        src/SdlPlatform.cpp
        src/SdlRenderer2D.cpp
    DEPS
        px::core
        px::render
        px::audio
        SDL3::SDL3)
