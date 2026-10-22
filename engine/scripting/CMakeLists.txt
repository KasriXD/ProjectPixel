# Creator scripting: Luau VM sandbox, resource limits, gameplay API and the
# Script component lifecycle. Depends on scene data but not on the runtime;
# the runtime provides game services through IScriptHost.
px_add_module(scripting
    SOURCES
        src/Api.cpp
        src/ScriptLibrary.cpp
        src/ScriptSystem.cpp
        src/ScriptVm.cpp
    DEPS
        px::core
        px::assets
        px::scene
    PRIVATE_DEPS
        px_luau)
