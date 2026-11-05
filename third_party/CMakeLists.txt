# Vendored single-header libraries. Versions and sources: third_party/VERSIONS.md

# stb_image: compiled once in its own translation unit, without project warnings.
add_library(px_stb_image STATIC stb/stb_image_impl.c)
target_include_directories(px_stb_image SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/stb")
set_target_properties(px_stb_image PROPERTIES POSITION_INDEPENDENT_CODE ON)

# stb_image_write: PNG encoder for re-encoding uploaded images (platform side).
add_library(px_stb_image_write STATIC stb/stb_image_write_impl.c)
target_include_directories(px_stb_image_write SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/stb")
set_target_properties(px_stb_image_write PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Monocypher: password hashing (Argon2), signatures (EdDSA), hashing (BLAKE2b).
add_library(px_monocypher STATIC monocypher/monocypher.c)
target_include_directories(px_monocypher SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/monocypher")
set_target_properties(px_monocypher PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Luau: scripting language compiler and VM.
add_subdirectory(luau)

# ENet: UDP transport for multiplayer (not for the browser).
if(PX_ENABLE_ENET)
    add_subdirectory(enet)
endif()

# The platform service and its clients: HTTP (cpp-httplib) and SQLite.
if(PX_ENABLE_PLATFORM)
    find_package(Threads REQUIRED)

    # cpp-httplib, split by its own split.py into httplib.h + httplib.cc so it
    # is compiled once. Plain HTTP only (no OpenSSL): see docs/platform.md.
    add_library(px_httplib STATIC httplib/httplib.cc)
    target_include_directories(px_httplib SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/httplib")
    target_compile_features(px_httplib PUBLIC cxx_std_17)
    target_link_libraries(px_httplib PUBLIC Threads::Threads)
    # WebSocket messages are game messages: snapshots stay below 8 MiB.
    target_compile_definitions(px_httplib PRIVATE CPPHTTPLIB_WEBSOCKET_MAX_PAYLOAD_LENGTH=9437184)
    if(WIN32)
        # cpp-httplib needs the Windows 10 API.
        target_compile_definitions(px_httplib PUBLIC _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN NOMINMAX)
        target_link_libraries(px_httplib PUBLIC ws2_32)
    endif()
    if(PX_ENABLE_TLS)
        # HTTPS and wss:// through Mbed TLS; trusted certificates come from the
        # system (Windows store, macOS keychain, Linux CA bundle).
        target_compile_definitions(px_httplib PUBLIC CPPHTTPLIB_MBEDTLS_SUPPORT PX_HAVE_TLS=1)
        target_link_libraries(px_httplib PUBLIC mbedtls mbedx509 mbedcrypto)
        if(WIN32)
            target_link_libraries(px_httplib PUBLIC crypt32)
        elseif(APPLE AND NOT IOS)
            # (iOS has no keychain API for the trusted roots: PlatformClient
            # uses a certificate file there, docs/launcher.md#phones-and-tablets.)
            target_compile_definitions(px_httplib PUBLIC CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN)
            target_link_libraries(px_httplib PUBLIC "-framework CoreFoundation" "-framework Security")
        endif()
    endif()

    # SQLite amalgamation: the platform's database.
    add_library(px_sqlite STATIC sqlite/sqlite3.c)
    target_include_directories(px_sqlite SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/sqlite")
    target_compile_definitions(px_sqlite PRIVATE
        SQLITE_THREADSAFE=2 SQLITE_DQS=0 SQLITE_DEFAULT_FOREIGN_KEYS=1 SQLITE_DEFAULT_MEMSTATUS=0
        SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_DEPRECATED SQLITE_OMIT_SHARED_CACHE SQLITE_LIKE_DOESNT_MATCH_BLOBS)
    target_link_libraries(px_sqlite PUBLIC Threads::Threads)
    if(UNIX)
        target_link_libraries(px_sqlite PUBLIC m)
    endif()
    if(MSVC)
        target_compile_options(px_sqlite PRIVATE /w)
    else()
        target_compile_options(px_sqlite PRIVATE -w)
    endif()
endif()

# doctest: header-only test framework.
add_library(px_doctest INTERFACE)
target_include_directories(px_doctest SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/doctest")

# Dear ImGui: Studio's user interface and the player's launcher.
if(PX_STUDIO_ENABLED OR PX_LAUNCHER_ENABLED)
    add_subdirectory(imgui)
endif()
