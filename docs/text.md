# Text

Games show text with the **Text** component: a sign in the level, a name
above a character, the score in a corner of the screen. Text is drawn with
bitmap fonts: pictures of letters, made once in Studio. Sound is in
[audio.md](audio.md); the decision behind fonts is
[ADR 0035](adr/0035-bitmap-fonts.md).

## The Text component

Studio: **Create > Text**, or drop a font in the Scene view. Fields:

| Field | Meaning |
|---|---|
| `text` | what it says, UTF-8, at most 2000 bytes; a new line starts a new line |
| `font` | a font asset; none means the built-in pixel font |
| `scale` | how big; whole numbers keep pixel fonts crisp |
| `color` | the letters' colour |
| `anchor` | which point of the text sits at the entity's position (see below) |
| `align` | how the lines of a text of several lines line up: `left`, `center`, `right` |
| `wrapWidth` | longer lines break at spaces (0: only at new lines) |
| `lineSpacing` | extra pixels between lines |
| `shadow` | a one-pixel drop shadow in this colour (transparent: none) |
| `background` | a box behind the text (transparent: none) |
| `padding` | around the text inside the box |
| `layer` | draw order, like sprites |
| `screen` | part of the screen (the HUD) instead of the world |
| `visible` | drawn or not |

The anchor is one of `top-left`, `top`, `top-right`, `left`, `center`,
`right`, `bottom-left`, `bottom`, `bottom-right` (Studio offers the list).

### In the world

A world text moves with its entity and is drawn among the sprites by its
layer. With anchor `bottom`, a text sits on its entity's position with its
middle: right for a name above a head.

### On the screen (the HUD)

A screen text stays put while the camera moves, and is drawn above the
world (among other screen texts by layer). Its anchor also chooses the point
of the screen it is measured from, and the entity's own position is the
distance from there: anchor `top-right` with position (-6, 6) puts the text
6 pixels in from the top-right corner, whatever the resolution.

In multiplayer, a screen text inside a player's character (a child of the
Player entity) is that player's HUD: only they see it. Other screen texts
are seen by everyone. Studio's Scene view shows screen texts inside the
frame of the game camera, as they will appear.

## Scripts

```lua
local score = World.find("Score")
score.text = "SCORE " .. Game.getScore()  -- what it says
score.textColor = "#ff8000"               -- "#rrggbb" or "#rrggbbaa"
score.visible = false                     -- hides its Text (and its Sprite)
```

Only authority scripts change texts; in multiplayer the change is sent to
every player like any other component. The examples' Level Rules script
shows the score in a Text named by its `scoreText` property.

## Fonts

### The built-in font

Every game has **Pixel Default**, an 8-pixel font baked from
[Press Start 2P](https://github.com/google/fonts/tree/main/ofl/pressstart2p)
by The Press Start 2P Project Authors, used under the SIL Open Font
License 1.1 (`engine/render/fonts/OFL.txt`). It has the Latin letters
(with Latin-1 and Latin Extended-A accents), Greek and Cyrillic. It is
compiled into the engine, so a game needs no files for it. Its lines are 10
pixels apart.

### Making a font

Import a `.ttf` or `.otf` file in Studio. Studio asks for a size in pixels
and whether the edges are smooth, then makes two assets in `fonts/`: the
letters as a picture (a texture asset) and the font (`*.font.json`) that
says where each letter is. Pixel mode turns every pixel fully on or off,
which keeps pixel fonts crisp at any scale; smooth mode keeps soft edges, for
fonts shown at the size they were made at. The letters made are Latin
(ASCII, Latin-1, Latin Extended-A), Greek, Cyrillic and a few punctuation
marks; the font's own name and the file are written into the font as its
source. Check that the font's licence allows embedding it in a game.

From the command line:

    px-headless --bake-font MyFont.ttf --size 16 --output fonts/myfont [--smooth] [--line-height 18]

writes `fonts/myfont.png` and `fonts/myfont.font.json` (add both to
`project.json`: a texture, then a font naming it with `"texture"`).

Characters a font lacks are shown as `?`, and so is broken UTF-8.

### The font file

```json
{
  "format": "px.font",
  "formatVersion": 1,
  "texture": "a texture asset id",
  "size": 8,
  "lineHeight": 10,
  "ascent": 8,
  "source": "what it was made from",
  "glyphs": [[65, 108, 19, 7, 7, 0, 0, 8]]
}
```

Each glyph is `[codepoint, x, y, width, height, offsetX, offsetY, advance]`:
the letter's rectangle in the texture, where its top-left goes relative to
the pen at the top of the line, and how far the pen then moves. Fonts have at
most 20000 glyphs and sizes up to 1024 pixels.

## Why bitmap fonts

Games never carry TrueType or OpenType files. Font files are complicated
programs for a rasterizer, and stb_truetype (which bakes fonts in Studio and
`px-headless --bake-font`) is not made for files from strangers. Players'
devices only read a picture and a list of rectangles; the upload check
checks every rectangle is inside its picture, and the picture is stored
again as PNG like every image. See [security.md](security.md).

## Limitations

- One size per font asset (make another for another size); no bold or
  italic of the same font, no kerning.
- Left-to-right scripts only: no Arabic or Hebrew shaping, no Chinese,
  Japanese or Korean in the built-in font (a baked font can hold them, up
  to its limits).
- Texts do not rotate with their entity.
- Clicking a text in the Scene view selects it by its position handle, not
  by its letters.
- The engine's own HUD (objective, messages, menus) still uses the debug
  font; games make their own HUD with Text.
