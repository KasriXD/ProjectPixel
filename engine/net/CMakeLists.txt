# Multiplayer networking: transports, the game protocol, snapshot
# replication, and the game server and client built on the runtime.
# Graphics-free like the runtime, so the dedicated server links it too.
px_add_module(net
    SOURCES
        src/Bytes.cpp
        src/Loopback.cpp
        src/Protocol.cpp
        src/Replication.cpp
    DEPS
        px::core
        px::scene
        px::project
        px::runtime)
