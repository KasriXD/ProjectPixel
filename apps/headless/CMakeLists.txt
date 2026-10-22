# px-headless: runs a project's simulation without any graphics or window.
# It is the seed of the dedicated server and is used for automated playthroughs.
add_executable(px_headless main.cpp)
set_target_properties(px_headless PROPERTIES OUTPUT_NAME px-headless)
target_link_libraries(px_headless PRIVATE px::runtime px::project)
px_target_defaults(px_headless)
