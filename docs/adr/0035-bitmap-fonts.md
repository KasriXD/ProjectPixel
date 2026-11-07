# ADR 0035: Text uses bitmap fonts baked in Studio; players never read TrueType

- Status: Accepted
- Date: 2026-11-07

## Context

Games need text: scores, signs, names, messages. The engine had only an
8x8 debug font in the renderer. Creators want their own fonts, and the
text has to look right in pixel-art games scaled up by whole numbers.
Fonts, like every asset, travel from creators to players' devices through
the platform.

## Decision

1. **Fonts are pictures of letters plus a list** (`*.font.json`: each
   glyph's rectangle in a texture asset, its offset and advance). The
   renderer draws text as sprites; layout (UTF-8, wrapping, alignment,
   anchors) is engine code with tests.
2. **Studio bakes TrueType and OpenType fonts** with stb_truetype
   (module `px_fontbake`), at one pixel size, in pixel mode (every pixel on
   or off) or smooth. Only Studio and `px-headless --bake-font` link it;
   it checks the font's table directory against the file first.
3. **A built-in font compiled into the engine**: Press Start 2P baked at
   8 px under the name Pixel Default (Latin, Greek, Cyrillic), as its SIL
   Open Font License allows for a modified version under another name, with
   the licence next to it.
4. **The Text component** carries the text and how it is shown; it is
   replicated like other components. Screen texts inside a player's
   character are drawn only for that player.
5. **The upload check checks fonts**: the font file parses with its limits
   and every rectangle lies inside its picture (which is stored again as
   PNG like every image).

## Alternatives considered

- **Shipping TrueType files and rasterizing on the device** (stb_truetype,
  FreeType): any size at runtime, but players' devices would parse complex
  files from strangers, stb_truetype states it is not for untrusted fonts,
  and FreeType is a large dependency. Pixel games want a few fixed sizes.
- **Signed distance field fonts**: smooth at every scale, but blurry for
  pixel art and need a shader the SDL renderer does not have.
- **Dear ImGui's font code for games**: tied to ImGui's renderer and its
  own atlas; games draw through the engine's renderer.

## Consequences

- Text costs one textured quad per letter and nothing else; it looks the
  same on every platform.
- A creator who wants two sizes makes two fonts. Kerning, right-to-left
  scripts and shaping (Arabic, Indic) are not supported.
- Fonts carry their licence obligations; Studio writes the font's name and
  file into the font so it can be credited.
