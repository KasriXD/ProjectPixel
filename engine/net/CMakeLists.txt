# Multiplayer networking: transports, the game protocol, snapshot
# replication, and the game server and client built on the runtime.
# Graphics-free like the runtime, so the dedicated server links it too.
px_add_module(net
    SOURCES
        src/Bytes.cpp
        src/CombinedEndpoint.cpp
        src/ConnectToken.cpp
        src/GameClient.cpp
        src/GameServer.cpp
        src/Loopback.cpp
        src/Protocol.cpp
        src/Replication.cpp
        src/Secure.cpp
        src/Udp.cpp
    DEPS
        px::core
        px::crypto
        px::scene
        px::project
        px::runtime)

# UDP (ENet) is optional: browsers cannot use it.
if(PX_ENABLE_ENET)
    target_link_libraries(px_net PRIVATE px_enet)
    target_compile_definitions(px_net PRIVATE PX_HAVE_ENET=1)
endif()

# WebSockets (browsers cannot use UDP): cpp-httplib on desktop and servers,
# the browser's own WebSocket in Emscripten builds.
if(EMSCRIPTEN)
    target_sources(px_net PRIVATE src/WebSocketBrowser.cpp)
    target_link_options(px_net PUBLIC -lwebsocket.js)
elseif(TARGET px_httplib)
    target_sources(px_net PRIVATE src/WebSocketNative.cpp)
    target_link_libraries(px_net PRIVATE px_httplib)
else()
    target_sources(px_net PRIVATE src/WebSocketNone.cpp)
endif()
