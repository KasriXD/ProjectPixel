# Executables. Executable names are brand-neutral (px-*) while the product
# name is temporary.

add_subdirectory(headless)

if(NOT EMSCRIPTEN)
    add_subdirectory(server)
endif()

if(PX_BUILD_CLIENT)
    add_subdirectory(player)
endif()

if(PX_STUDIO_ENABLED)
    add_subdirectory(studio)
endif()
