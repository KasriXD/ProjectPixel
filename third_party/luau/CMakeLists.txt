# Vendored Luau (MIT). Version and commit: VERSION / sources.cmake.
# Update with tools/update_luau.py; see docs/dependencies.md#luau.
#
# Only the libraries needed to compile and run scripts are built:
# Common, Ast, Bytecode, Compiler and VM. Native code generation (CodeGen)
# is deliberately excluded: it is unavailable on iOS (no JIT) and in browsers.

include("${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake")
set(LUAU_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

function(px_luau_library name dir)
    add_library(${name} STATIC ${ARGN})
    target_include_directories(${name} SYSTEM PUBLIC "${LUAU_DIR}/${dir}/include")
    target_compile_features(${name} PUBLIC cxx_std_17)
    set_target_properties(${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${name} PRIVATE /W1)
    else()
        target_compile_options(${name} PRIVATE -w) # third-party code: do not report warnings
    endif()
    if(EMSCRIPTEN)
        # Luau reports script errors with C++ exceptions.
        target_compile_options(${name} PUBLIC -fexceptions)
        target_link_options(${name} PUBLIC -fexceptions)
    endif()
endfunction()

list(TRANSFORM PX_LUAU_COMMON_SOURCES PREPEND "${LUAU_DIR}/")
list(TRANSFORM PX_LUAU_AST_SOURCES PREPEND "${LUAU_DIR}/")
list(TRANSFORM PX_LUAU_BYTECODE_SOURCES PREPEND "${LUAU_DIR}/")
list(TRANSFORM PX_LUAU_COMPILER_SOURCES PREPEND "${LUAU_DIR}/")
list(TRANSFORM PX_LUAU_VM_SOURCES PREPEND "${LUAU_DIR}/")

px_luau_library(px_luau_common Common ${PX_LUAU_COMMON_SOURCES})
px_luau_library(px_luau_ast Ast ${PX_LUAU_AST_SOURCES})
target_link_libraries(px_luau_ast PUBLIC px_luau_common)
px_luau_library(px_luau_bytecode Bytecode ${PX_LUAU_BYTECODE_SOURCES})
target_link_libraries(px_luau_bytecode PUBLIC px_luau_common)
px_luau_library(px_luau_compiler Compiler ${PX_LUAU_COMPILER_SOURCES})
target_link_libraries(px_luau_compiler PUBLIC px_luau_ast px_luau_bytecode)
px_luau_library(px_luau_vm VM ${PX_LUAU_VM_SOURCES})
target_link_libraries(px_luau_vm PUBLIC px_luau_common)

if(NOT MSVC)
    # Lets sqrt() compile to a single instruction (matches upstream).
    target_compile_options(px_luau_vm PRIVATE -fno-math-errno)
endif()
if(MSVC AND MSVC_VERSION GREATER_EQUAL 1924)
    # Upstream workaround for an interpreter performance regression in VS2022.
    set_source_files_properties("${LUAU_DIR}/VM/src/lvmexecute.cpp" PROPERTIES COMPILE_FLAGS /d2ssa-pre-)
endif()

# Everything the engine uses: compiler (source -> bytecode) and VM.
add_library(px_luau INTERFACE)
target_link_libraries(px_luau INTERFACE px_luau_compiler px_luau_vm)

message(STATUS "Luau ${PX_LUAU_VERSION} (${PX_LUAU_COMMIT}) vendored")
