# ENet 1.3.18: reliable and unreliable messages over UDP, used by px_net for
# desktop (and later mobile) multiplayer. Not built for the browser, which
# cannot open UDP sockets. Sources are vendored unmodified; this file is ours
# (ENet's own CMakeLists.txt needs an older CMake than we allow).
add_library(px_enet STATIC
    callbacks.c compress.c host.c list.c packet.c peer.c protocol.c unix.c win32.c)
target_include_directories(px_enet SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(px_enet PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(WIN32)
    target_link_libraries(px_enet PUBLIC ws2_32 winmm)
    target_compile_definitions(px_enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS _CRT_SECURE_NO_WARNINGS)
else()
    # The platform checks ENet's own build performs, for unix.c.
    include(CheckFunctionExists)
    include(CheckStructHasMember)
    include(CheckTypeSize)
    check_function_exists("fcntl" PX_ENET_HAS_FCNTL)
    check_function_exists("poll" PX_ENET_HAS_POLL)
    check_function_exists("getaddrinfo" PX_ENET_HAS_GETADDRINFO)
    check_function_exists("getnameinfo" PX_ENET_HAS_GETNAMEINFO)
    check_function_exists("gethostbyname_r" PX_ENET_HAS_GETHOSTBYNAME_R)
    check_function_exists("gethostbyaddr_r" PX_ENET_HAS_GETHOSTBYADDR_R)
    check_function_exists("inet_pton" PX_ENET_HAS_INET_PTON)
    check_function_exists("inet_ntop" PX_ENET_HAS_INET_NTOP)
    check_struct_has_member("struct msghdr" "msg_flags" "sys/types.h;sys/socket.h" PX_ENET_HAS_MSGHDR_FLAGS)
    set(CMAKE_EXTRA_INCLUDE_FILES "sys/types.h" "sys/socket.h")
    check_type_size("socklen_t" PX_ENET_HAS_SOCKLEN_T BUILTIN_TYPES_ONLY)
    unset(CMAKE_EXTRA_INCLUDE_FILES)
    foreach(feature FCNTL POLL GETADDRINFO GETNAMEINFO GETHOSTBYNAME_R GETHOSTBYADDR_R INET_PTON INET_NTOP
                    MSGHDR_FLAGS SOCKLEN_T)
        if(PX_ENET_HAS_${feature})
            target_compile_definitions(px_enet PRIVATE HAS_${feature}=1)
        endif()
    endforeach()
endif()
