# Turns TrueType and OpenType fonts into bitmap fonts (docs/text.md#fonts).
# Only Studio and px-headless --bake-font link it: font files are complex and
# stb_truetype is not made for untrusted ones, so games carry baked fonts and
# nothing that runs a downloaded game or checks an upload reads TrueType.
px_add_module(fontbake
    SOURCES
        src/FontBaker.cpp
    DEPS
        px::core
        px::assets
        px::project
    PRIVATE_DEPS
        px_stb_truetype)
