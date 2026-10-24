# The runtime is the shared game simulation used by the client and, later,
# the dedicated server. It must never depend on graphics, windowing or audio.
px_add_module(runtime
    SOURCES
        src/GameLoader.cpp
        src/GameSession.cpp
        src/Input.cpp
        src/InputScript.cpp
        src/Movement.cpp
    DEPS
        px::core
        px::scene
        px::physics
        px::project
        px::scripting)
