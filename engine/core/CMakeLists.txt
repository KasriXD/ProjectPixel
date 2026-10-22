configure_file(Version.hpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/px/core/Version.hpp" @ONLY)

px_add_module(core
    SOURCES
        src/Color.cpp
        src/FileSystem.cpp
        src/Guid.cpp
        src/Keys.cpp
        src/Log.cpp
        src/Result.cpp
        src/Time.cpp)

target_include_directories(px_core PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/generated")
