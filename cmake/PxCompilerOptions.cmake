# Shared compiler settings for Project Pixel targets.
# Third-party code is NOT built with these flags.

function(px_target_defaults target)
    target_compile_features(${target} PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor
            /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14826 /w14905)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
        if(PX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align
            -Wunused -Woverloaded-virtual -Wnull-dereference -Wformat=2
            -Wimplicit-fallthrough -Wmisleading-indentation)
        if(PX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    if(PX_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
        target_link_options(${target} PUBLIC -fsanitize=address,undefined)
    endif()
endfunction()

# px_add_module(<name> SOURCES ... DEPS ... [PRIVATE_DEPS ...])
# Creates static library px_<name> with public headers in include/ (px/<name>/...).
function(px_add_module name)
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})
    set(target px_${name})
    add_library(${target} STATIC ${ARG_SOURCES})
    add_library(px::${name} ALIAS ${target})
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    if(ARG_DEPS)
        target_link_libraries(${target} PUBLIC ${ARG_DEPS})
    endif()
    if(ARG_PRIVATE_DEPS)
        target_link_libraries(${target} PRIVATE ${ARG_PRIVATE_DEPS})
    endif()
    px_target_defaults(${target})
endfunction()
