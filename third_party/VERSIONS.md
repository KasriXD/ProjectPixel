# Vendored third-party code

| Library | Version | Source | License | Used for |
|---|---|---|---|---|
| stb_image.h | v2.30, commit `a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d` | https://github.com/nothings/stb | MIT or public domain (`stb/LICENSE`) | Decoding PNG/JPEG/BMP/TGA textures in `px_assets` |
| Luau (Common, Ast, Bytecode, Compiler, VM) | 0.743, commit `9d7748a4f324213a18cb8283fa2a77c330225fa0` | https://github.com/luau-lang/luau | MIT (`luau/LICENSE.txt`, `luau/lua_LICENSE.txt`) | Script compiler and VM in `px_scripting`. Update with `tools/update_luau.py` |
| doctest.h | v2.5.3 | https://github.com/doctest/doctest | MIT (`doctest/LICENSE.txt`) | Unit test framework |

Files are copied unmodified (Luau's build file `luau/CMakeLists.txt` is ours). To update, replace the header, update this table,
rebuild, and run the full test suite.

Fetched (not vendored) dependencies are pinned in `cmake/PxDependencies.cmake`.
