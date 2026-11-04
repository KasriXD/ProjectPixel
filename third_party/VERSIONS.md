# Vendored third-party code

| Library | Version | Source | License | Used for |
|---|---|---|---|---|
| stb_image.h | v2.30, commit `a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d` | https://github.com/nothings/stb | MIT or public domain (`stb/LICENSE`) | Decoding PNG/JPEG/BMP/TGA textures in `px_assets` |
| Luau (Common, Ast, Bytecode, Compiler, VM) | 0.743, commit `9d7748a4f324213a18cb8283fa2a77c330225fa0` | https://github.com/luau-lang/luau | MIT (`luau/LICENSE.txt`, `luau/lua_LICENSE.txt`) | Script compiler and VM in `px_scripting`. Update with `tools/update_luau.py` |
| Dear ImGui (core, SDL3 and SDL_Renderer backends, imgui_stdlib) | v1.92.9b-docking, commit `b48d1afbe8ee8b238e2961dc363a949dd7304e23` | https://github.com/ocornut/imgui | MIT (`imgui/LICENSE.txt`) | Studio's UI (`apps/studio`). Update with `tools/update_imgui.py` |
| ENet | v1.3.18, commit `2662c0de09e36f2a2030ccc2c528a3e4c9e8138a` | https://github.com/lsalzman/enet | MIT (`enet/LICENSE`) | UDP transport for multiplayer in `px_net` (not in WebAssembly builds) |
| cpp-httplib (`httplib.h`, `httplib.cc` made by its `split.py`) | v0.62.0, commit `74ef3c42188acd6391f027c4337fb0e1a68c0024` | https://github.com/yhirose/cpp-httplib | MIT (`httplib/LICENSE`) | HTTP client and server for the platform (`px_online`, `services/platform`) |
| SQLite amalgamation (`sqlite3.c`, `sqlite3.h`) | 3.54.1, tag `version-3.54.1`, commit `57520227c95396ce39ba6ecaac8d475b78dd3964`, built with `make sqlite3.c` | https://github.com/sqlite/sqlite | Public domain | The platform's database (`services/platform`) |
| Monocypher (`monocypher.c`, `monocypher.h`) | 4.0.3, commit `ab2b16dd619ad5f6979a4fbe69cfa324a6fcc35f` | https://github.com/LoupVaillant/Monocypher | BSD-2-Clause or CC0 (`monocypher/LICENCE.md`) | Passwords, signatures and hashes in `px_crypto` |
| stb_image_write.h | v1.16, commit `a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d` | https://github.com/nothings/stb | MIT or public domain (`stb/LICENSE`) | Re-encoding uploaded images as PNG (`px_online`) |
| doctest.h | v2.5.3 | https://github.com/doctest/doctest | MIT (`doctest/LICENSE.txt`) | Unit test framework |

Files are copied unmodified (the build files `luau/CMakeLists.txt`, `imgui/CMakeLists.txt` and `enet/CMakeLists.txt` are ours). To update, replace the header, update this table,
rebuild, and run the full test suite.

Fetched (not vendored) dependencies are pinned in `cmake/PxDependencies.cmake`.
