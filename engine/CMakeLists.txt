# Engine modules, listed in dependency order. Each module is a static library
# px_<name> with headers under <module>/include/px/<name>/.
#
#   core      - logging, errors, math, ids, file access, timing, key names
#   assets    - asset ids, asset registry, image decoding
#   scene     - entities, components, scenes
#   physics   - collision world (kinematic AABB movement, triggers)
#   project   - versioned project/scene file formats (JSON)
#   scripting - Luau sandbox, limits, gameplay API, Script lifecycle
#   runtime   - headless game simulation (fixed tick, input actions, gameplay systems)
#   render    - renderer interface, camera, scene-to-draw-call extraction (no backend)
#   platform_sdl - SDL3 window/input/renderer backend (client only)

add_subdirectory(core)
add_subdirectory(assets)
add_subdirectory(scene)
add_subdirectory(physics)
add_subdirectory(project)
add_subdirectory(scripting)
add_subdirectory(runtime)
add_subdirectory(render)

if(PX_BUILD_CLIENT)
    add_subdirectory(platform_sdl)
endif()
