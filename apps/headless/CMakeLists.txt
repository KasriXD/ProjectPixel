# px-headless: runs a project's simulation without any graphics or window.
# It is the seed of the dedicated server and is used for automated playthroughs.
add_executable(px_headless main.cpp NetTools.cpp)
set_target_properties(px_headless PROPERTIES OUTPUT_NAME px-headless)
target_link_libraries(px_headless PRIVATE px::runtime px::project px::net)
px_target_defaults(px_headless)
if(PX_ENABLE_PLATFORM)
    target_link_libraries(px_headless PRIVATE px::online)
    target_compile_definitions(px_headless PRIVATE PX_HAVE_PLATFORM=1)
endif()

if(EMSCRIPTEN)
    # Runs under Node.js with direct access to the host file system.
    target_link_options(px_headless PRIVATE -sNODERAWFS=1 -sEXIT_RUNTIME=1)
endif()
