# Cryptography for the platform: secure random numbers, hashing, password
# hashing and signatures (Monocypher, private). Graphics- and network-free,
# so the game server and the browser build can verify connect tokens.
px_add_module(crypto
    SOURCES
        src/Crypto.cpp
        src/Encoding.cpp
    DEPS
        px::core
    PRIVATE_DEPS
        px_monocypher)

if(WIN32)
    target_link_libraries(px_crypto PRIVATE bcrypt)
endif()
