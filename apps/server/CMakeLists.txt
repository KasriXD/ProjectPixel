# px-server: the dedicated multiplayer server (no graphics, no window).
add_executable(px_server main.cpp)
set_target_properties(px_server PROPERTIES OUTPUT_NAME px-server)
target_link_libraries(px_server PRIVATE px::net px::runtime px::project)
if(WIN32)
    target_link_libraries(px_server PRIVATE winmm)
endif()
px_target_defaults(px_server)
