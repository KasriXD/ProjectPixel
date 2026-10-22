cmake_minimum_required(VERSION 3.24)

# ---------------------------------------------------------------------------
# Naming: the product name is temporary. Code uses the neutral "px" prefix;
# the user-facing name lives only in PX_PRODUCT_NAME (see docs/adr/0001).
# ---------------------------------------------------------------------------
set(PX_PRODUCT_NAME "Project Pixel" CACHE STRING "User-facing product name (temporary)")

# Apple's libc++ only provides floating-point std::to_chars / std::format from macOS 13.3.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3" CACHE STRING "Minimum macOS version")

project(px
    VERSION 0.2.0
    DESCRIPTION "2D user-generated game platform: engine, runtime and tools"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(PX_BUILD_CLIENT "Build the SDL3 client (player). OFF gives a headless, graphics-free build." ON)
option(PX_BUILD_TESTS "Build the automated test suite" ON)
option(PX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(PX_ENABLE_SANITIZERS "Build with AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# WebAssembly (Emscripten) builds - experimental, see docs/platform-support.md#browser.
if(EMSCRIPTEN)
    # Luau and the project-file parser report errors with C++ exceptions,
    # which Emscripten disables unless asked.
    add_compile_options(-fexceptions)
    add_link_options(-fexceptions -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576)
endif()
include(PxCompilerOptions)
include(PxDependencies)

# Location of the bundled example projects, used for dev defaults and tests.
set(PX_EXAMPLES_DIR "${CMAKE_CURRENT_SOURCE_DIR}/examples")

add_subdirectory(third_party)
add_subdirectory(engine)
add_subdirectory(apps)

if(PX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "${PX_PRODUCT_NAME} ${PROJECT_VERSION}: client=${PX_BUILD_CLIENT} tests=${PX_BUILD_TESTS} sanitizers=${PX_ENABLE_SANITIZERS}")
