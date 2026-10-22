# Cross-compile for 64-bit Windows from Linux with MinGW-w64 (posix threads).
# Ubuntu: sudo apt install g++-mingw-w64-x86-64-posix
# Usage:  cmake -S . -B build/mingw -G Ninja -DCMAKE_BUILD_TYPE=Release \
#           -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Link the MinGW C/C++ runtime statically so the .exe files run without
# libstdc++/libgcc/winpthread DLLs next to them.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static")
