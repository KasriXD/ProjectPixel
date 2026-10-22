# Vendored third-party code

| Library | Version | Source | License | Used for |
|---|---|---|---|---|
| stb_image.h | v2.30, commit `a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d` | https://github.com/nothings/stb | MIT or public domain (`stb/LICENSE`) | Decoding PNG/JPEG/BMP/TGA textures in `px_assets` |
| doctest.h | v2.5.3 | https://github.com/doctest/doctest | MIT (`doctest/LICENSE.txt`) | Unit test framework |

Files are copied unmodified. To update, replace the header, update this table,
rebuild, and run the full test suite.

Fetched (not vendored) dependencies are pinned in `cmake/PxDependencies.cmake`.
