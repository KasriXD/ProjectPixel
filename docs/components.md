# Component reference

Generated from the component descriptors in `engine/scene/src/ComponentInfo.cpp` with
`px-headless --print-component-reference`; do not edit by hand. The scene file
format, Studio's inspector and this page all come from the same table.

In scene files each component is an object of fields under the entity's
`components` (see [project-format.md](project-format.md)); fields that are left
out take their default. Runtime fields are simulation state: they are not
saved, and Studio shows them read-only while the game is playing.

## Transform

*General.* Position, rotation and scale.

| Field | Type | Default | Allowed values | Description |
|---|---|---|---|---|
| `position` | vector | [0, 0] |  | Position relative to the parent, in world units (pixels at zoom 1). Y points down. |
| `rotation` | number | 0 |  | Rotation in degrees, clockwise, relative to the parent. |
| `scale` | vector | [1, 1] |  | Scale relative to the parent. Negative values mirror. |

## Sprite

*Rendering.* Draws an image (or part of one) at the entity's position.

| Field | Type | Default | Allowed values | Description |
|---|---|---|---|---|
| `texture` | asset | required | a texture asset | The image to draw. |
| `source` | rectangle | [0, 0, 0, 0] |  | Part of the image to draw, in texels [x, y, w, h]. Empty = the whole image. |
| `size` | vector | [16, 16] | must be positive | Size in world units before scaling. |
| `pivot` | vector | [0.5, 0.5] |  | Point of the sprite placed at the entity's position: [0, 0] = top-left, [0.5, 0.5] = centre. |
| `tint` | colour | #ffffff |  | Colour multiplied with the image; alpha makes it transparent. |
| `layer` | integer | 0 |  | Draw order: lower layers are drawn first (further back). |
| `flipX` | boolean | false |  | Mirror horizontally. |
| `flipY` | boolean | false |  | Mirror vertically. |
| `visible` | boolean | true |  | Whether the sprite is drawn. |

## SpriteAnimation

*Rendering.* Plays frames on the entity's Sprite by changing its source rectangle.

| Field | Type | Default | Allowed values | Description |
|---|---|---|---|---|
| `frames` | list of rectangles | required | 1 to 1024 entries | Frames as texel rectangles [x, y, w, h] of the sprite's image, in playing order. |
| `framesPerSecond` | number | 8 | must be greater than 0 and at most 240 | Playback speed. |
| `loop` | boolean | true |  | Start again after the last frame. |
| `playing` | boolean | true |  | Whether the animation plays when the game starts. |

Runtime: `time` (Seconds played so far.)

## Animator

*Rendering.* Plays animation clips on the Sprite: idle, run, jump and fall follow a controller's movement; scripts can play any clip.

Requires Sprite on the same entity.

| Field | Type | Default | Allowed values | Description |
|---|---|---|---|---|
| `clip` | asset | none | a animation asset | Plays when no other clip applies (no controller, or its clip is not set). |
| `idle` | asset | none | a animation asset | With a controller: standing still. |
| `run` | asset | none | a animation asset | With a controller: moving (on the ground). |
| `jump` | asset | none | a animation asset | With a PlatformerController: going up in the air. |
| `fall` | asset | none | a animation asset | With a PlatformerController: coming down in the air. |
| `speed` | number | 1 | must be between 0 and 16 | Playback speed: 1 plays the clips at their own speed, 2 twice as fast. |
| `playing` | boolean | true |  | Whether clips advance (false holds the current frame). |

Runtime: `current` (The clip playing now.), `scripted` (The clip a script chose with entity:playAnimation (none: automatic).), `time` (Seconds into the current clip.)

## Tilemap

*Rendering.* A grid of tiles from a tileset image; can be solid ground.

| Field | Type | Default | Allowed values | Description |
|---|---|---|---|---|
| `tileset` | asset | required | a texture asset | Image with the tiles, in cells of tileSize, numbered left to right, top to bottom from 0. |
| `tileSize` | vector | [16, 16] | must be positive | Size of one tile in world units and in tileset texels. |
| `tilesetColumns` | integer | 1 | must be at least 1 | Number of tile columns in the tileset image. |
| `solid` | boolean | true |  | Every non-empty tile blocks moving bodies. |
| `layer` | integer | 0 |  | Draw order: lower layers are drawn first (further back). |
| `tint` | colour | #ffffff |  | Colour multiplied with every tile. |
| `rows` | tile grid | required |  | The tiles, one string per row: tile numbers or '.' for empty, separated by spaces. The top-left tile is at the entity's position. |

## BoxCollider

*Physics.* An axis-aligned box. Solid boxes block moving bodies; triggers report overlaps.

| Field | Type | Default | Allowed values | Description |
|---|---|---|---|---|
| `size` | vector | [16, 16] | must be positive | Width and height of the box. |
| `offset` | vector | [0, 0] |  | Centre of the box relative to the entity's position. |
| `isTrigger` | boolean | false |  | Triggers do not block; they report overlaps to scripts (TriggerEnter/TriggerExit). |
| `oneWay` | boolean | false |  | A solid box that is only a floor: bodies jump up through it, walk through it sideways and land on top (platforms). |

## KinematicBody

*Physics.* Moves by its velocity and stops at solid tiles and boxes (needs a BoxCollider and a Transform to move).

No saved fields: `{}`.

Runtime: `velocity` (Units per second.), `onGround` (Standing on something.), `hitCeiling` (Bumped into something above this tick.), `hitWall` (Bumped into something sideways this tick.)

## PlatformerController

*Gameplay.* Side-view running and jumping driven by the move_left, move_right and jump input actions.

Requires Transform, BoxCollider and KinematicBody on the same entity.

| Field | Type | Default | Allowed values | Description |
|---|---|---|---|---|
| `maxSpeed` | number | 110 | may not be negative | Top running speed, units per second. |
| `acceleration` | number | 1200 | may not be negative | Speed gained per second on the ground. |
| `deceleration` | number | 1600 | may not be negative | Speed lost per second on the ground with no input. |
| `airAcceleration` | number | 900 | may not be negative | Speed gained per second in the air. |
| `jumpSpeed` | number | 300 | may not be negative | Initial upward speed of a jump. |
| `gravity` | number | 900 | may not be negative | Downward acceleration, units per second squared. |
| `maxFallSpeed` | number | 420 | may not be negative | Falling speed limit. |
| `jumpCutMultiplier` | number | 0.44999998807907104 | must be between 0 and 1 | Fraction of upward speed kept when the jump button is released early. |
| `coyoteTime` | number | 0.07999999821186066 | may not be negative | Seconds after walking off a ledge during which jumping still works. |
| `jumpBufferTime` | number | 0.10000000149011612 | may not be negative | Seconds a jump press is remembered before landing. |

Runtime: `coyoteTimer` (Coyote time left.), `jumpBufferTimer` (Buffered jump time left.), `jumping` (Rising from a jump.)

## TopDownController

*Gameplay.* Top-down walking in all directions, driven by the move_left, move_right, move_up and move_down input actions.

Requires Transform, BoxCollider and KinematicBody on the same entity.

| Field | Type | Default | Allowed values | Description |
|---|---|---|---|---|
| `maxSpeed` | number | 80 | may not be negative | Top walking speed, units per second (also diagonally). |
| `acceleration` | number | 900 | may not be negative | Speed gained per second while a direction is held. |
| `deceleration` | number | 1200 | may not be negative | Speed lost per second with no input. |

Runtime: `facing` (Last direction moved in.)

## Player

*Gameplay.* Marks the player character; it respawns at its start position after falling below the kill plane. In multiplayer every player who joins gets a copy of it.

No saved fields: `{}`.

Runtime: `spawnPoint` (Where the player respawns (its position when the game started).), `owner` (In multiplayer, the id of the player controlling this character (0 when playing alone).), `name` (In multiplayer, the controlling player's name.)

## Camera

*Rendering.* The view of the game. The first camera in the scene is used.

| Field | Type | Default | Allowed values | Description |
|---|---|---|---|---|
| `follow` | entity | none |  | Entity to keep in the centre of the view (none: the camera's own position). |
| `zoom` | number | 1 | must be greater than 0 and at most 64 | Screen pixels per world unit. |
| `clampToBounds` | boolean | true |  | Keep the view inside the level (the union of all tilemaps). |

Runtime: `position` (World point at the centre of the view.)

## Tags

*Gameplay.* Labels for finding and classifying entities from scripts.

Written as a list of text directly: `"Tags": [...]`. Labels scripts can search for (World.findWithTag) and test (entity:hasTag).

## Scripts

*Scripting.* Luau behaviours that run on this entity.

Written as a list of scripts directly: `"Scripts": [...]`. Luau behaviours, each with its property values. They run in this order.

## AudioSource

*Audio.* Plays a sound from the entity while `playing` is true: a loop in the world (a waterfall, a machine) or the music of a place.

| Field | Type | Default | Allowed values | Description |
|---|---|---|---|---|
| `sound` | asset | none | a sound asset | The sound to play. |
| `volume` | number | 1 | must be between 0 and 4 | 1 plays it as it is, 0.5 at half volume. |
| `pitch` | number | 1 | must be between 0.25 and 4 | Playback speed: 2 is an octave up and twice as fast. |
| `loop` | boolean | true |  | Starts again at the end (false: plays once each time it starts). |
| `playing` | boolean | true |  | Plays while true; scripts switch it (entity.soundPlaying). |
| `positional` | boolean | true |  | Heard from where the entity is: louder on screen, panned to its side. False: the same everywhere. |
| `range` | number | 0 | may not be negative | Positional: silent this far from the camera's centre, in world units (0: full volume on screen, fading over half a screen beyond it). |

## Text

*Rendering.* Text drawn with a bitmap font: in the world, or on the screen as part of the HUD.

| Field | Type | Default | Allowed values | Description |
|---|---|---|---|---|
| `text` | text | "" |  | What it says (UTF-8; a new line starts a new line). |
| `font` | asset | none | a font asset | A font asset (none: the built-in 8-pixel font). |
| `scale` | number | 1 | must be greater than 0 and at most 64 | Size: 2 draws every pixel of the font as 2 by 2 (whole numbers stay crisp). |
| `color` | colour | #ffffff |  | The colour of the letters. |
| `anchor` | text | "top-left" | `top-left`, `top`, `top-right`, `left`, `center`, `right`, `bottom-left`, `bottom`, `bottom-right` | The point of the text placed at the entity's position. Screen texts: also the corner or edge of the screen the position is measured from. |
| `align` | text | "left" | `left`, `center`, `right` | How the lines of a text of several lines line up. |
| `wrapWidth` | number | 0 | may not be negative | Lines longer than this break between words, in font pixels (0: only where the text has new lines). |
| `lineSpacing` | number | 0 | must be between -64 and 256 | Extra space between lines, in font pixels. |
| `shadow` | colour | #00000000 |  | A drop shadow one pixel down and right (transparent: none). |
| `background` | colour | #00000000 |  | A box behind the text (transparent: none). |
| `padding` | number | 2 | must be between 0 and 256 | Space around the text inside the box, in font pixels. |
| `layer` | integer | 100 |  | Draw order: higher layers are drawn later (on top). Screen texts are above the world and ordered among themselves. |
| `screen` | boolean | false |  | Part of the screen (a HUD) instead of the world: the position is in screen pixels from the anchor. Inside a player's character only that player sees it. |
| `visible` | boolean | true |  | Whether it is drawn. |

## PrefabInstance

*General.* Marks a copy of a prefab. Studio updates it when the prefab changes; removing it unlinks the copy.

| Field | Type | Default | Allowed values | Description |
|---|---|---|---|---|
| `prefab` | asset | required | a prefab asset | The prefab this entity and its children were made from. |
