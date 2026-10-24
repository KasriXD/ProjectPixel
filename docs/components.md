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
