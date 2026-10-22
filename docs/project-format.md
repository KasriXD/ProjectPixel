# Project format (version 1)

A game project is a folder:

```
my-game/
  project.json            manifest (required, this exact name)
  scenes/*.scene.json     scenes
  assets/...              textures and other assets
```

Every file is UTF-8 JSON. Loading is **strict**: unknown fields, wrong types
and out-of-range values are errors that name the file and the JSON path, with
a spelling suggestion when a field or component name is close to a known one.

All paths inside a project are relative, use `/`, and may not contain `..`,
`:` or a leading `/`. Symbolic links that resolve outside the project folder
are refused.

## Identifiers

Assets, scenes, entities and projects are identified by GUIDs
(`"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"`). Files refer to each other only by
GUID; file paths appear in exactly one place, the manifest's asset list. Moving
or renaming a file therefore means editing one `path` entry.

## project.json

```json
{
  "format": "px.project",
  "formatVersion": 1,
  "id": "af5d3a02-6054-58d1-a81c-23a97281ae0f",
  "name": "Example Platformer",
  "engineVersion": "0.1.0",
  "startScene": "e0464a98-2756-52ff-8acd-37c81877ff1a",
  "settings": {
    "tickRate": 60,
    "display": { "virtualResolution": [320, 180], "scaling": "integer", "windowScale": 3 }
  },
  "input": {
    "actions": {
      "move_left": ["Left", "A"],
      "move_right": ["Right", "D"],
      "jump": ["Space", "Z", "Up", "W"]
    }
  },
  "assets": [
    { "id": "782c3432-...", "type": "texture", "name": "Tiles", "path": "assets/tiles.png", "filter": "nearest" },
    { "id": "e0464a98-...", "type": "scene", "name": "Level 1", "path": "scenes/level1.scene.json" }
  ]
}
```

| Field | Required | Meaning |
|---|---|---|
| `format`, `formatVersion` | yes | `"px.project"`, `1` |
| `id` | yes | project GUID |
| `name` | yes | display name, not empty |
| `engineVersion` | no | engine version that last saved the file (informational) |
| `startScene` | yes | GUID of a `scene` asset |
| `settings.tickRate` | no (60) | fixed simulation steps per second, 1-240 |
| `settings.display.virtualResolution` | no ([320, 180]) | logical screen size in pixels |
| `settings.display.scaling` | no (`"integer"`) | `"integer"` (whole-number scale, crisp pixels) or `"letterbox"` |
| `settings.display.windowScale` | no (3) | initial desktop window size multiplier, 1-16 |
| `input.actions` | no | action name → list of default keys. At most 32 actions; names use `a-z`, `0-9`, `_`. Order defines action ids. |
| `assets` | no | asset list, see below; at most 10 000 entries |

Key names: `A`-`Z`, `0`-`9`, `Space`, `Enter`, `Escape`, `Backspace`, `Tab`,
`Left`, `Right`, `Up`, `Down`, `LeftShift`, `RightShift`, `LeftCtrl`,
`RightCtrl`, `LeftAlt`, `RightAlt`, `F1`-`F12` (case-insensitive). Keys are
physical positions, so `W`/`A`/`S`/`D` stay in place on non-QWERTY layouts.

Standard actions used by built-in components: `move_left`, `move_right`,
`move_up`, `move_down`, `jump`.

### Assets

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | asset GUID, unique |
| `type` | yes | `"texture"` or `"scene"` |
| `path` | yes | file path inside the project |
| `name` | no | label for tools (defaults to the path) |
| `filter` | no (`"nearest"`) | textures only: `"nearest"` for pixel art, `"linear"` for smooth art |

Textures may be PNG, JPEG, BMP or TGA, at most 8192x8192 pixels.

## Scene files

```json
{
  "format": "px.scene",
  "formatVersion": 1,
  "name": "Level 1",
  "settings": { "backgroundColor": "#9bd4f0", "killPlaneY": 264 },
  "entities": [
    {
      "id": "78e3a2b6-c252-54d7-a6b1-3f2e391edac0",
      "name": "Player",
      "components": {
        "Transform": { "position": [56, 168] },
        "Sprite": { "texture": "2e723cc5-...", "size": [16, 16], "layer": 10 },
        "BoxCollider": { "size": [10, 14], "offset": [0, 1] },
        "KinematicBody": {},
        "PlatformerController": { "maxSpeed": 110, "jumpSpeed": 300 },
        "Player": {}
      }
    }
  ]
}
```

| Field | Required | Meaning |
|---|---|---|
| `name` | no | scene display name |
| `settings.backgroundColor` | no | clear colour, `"#rrggbb"` or `"#rrggbbaa"` |
| `settings.killPlaneY` | no | players whose Y exceeds this are respawned at their start position |
| `entities` | yes | at most 100 000 entities; each has `id` (GUID, unique in the scene), optional `name`, optional `components` |

Units: world units, where 1 unit is 1 pixel at zoom 1. Y points **down**.
Angles are degrees, clockwise. Vectors are `[x, y]`; rectangles are
`[x, y, width, height]` with `(x, y)` the top-left corner.

### Components

Defaults are shown; every field is optional unless marked *required*.

**Transform**: `position [0, 0]`, `rotation 0`, `scale [1, 1]`.

**Sprite**: `texture` (*required*, texture asset id), `source [0, 0, 0, 0]`
(texel rectangle; empty = whole texture), `size [16, 16]` (world size before
scale), `pivot [0.5, 0.5]` (point of the sprite placed at the position),
`tint "#ffffff"`, `layer 0` (lower draws first), `flipX false`, `flipY false`,
`visible true`.

**SpriteAnimation**: `frames` (*required*, non-empty list of rectangles, at
most 1024), `framesPerSecond 8` (0-240, exclusive of 0), `loop true`,
`playing true`. Sets the entity's `Sprite.source` each tick.

**Tilemap**: `tileset` (*required*, texture asset id), `tileSize [16, 16]`,
`tilesetColumns 1`, `solid true` (every non-empty tile collides), `layer 0`,
`tint "#ffffff"`, `rows` (*required*). Each row is a string of
space-separated tokens: `.` for empty or a tile index (left to right, top to
bottom in the tileset). All rows must have the same number of tokens; at most
4096 x 4096 tiles. `Transform.position` is the map's top-left corner.
In version 1 a tileset cell is `tileSize` texels; tileset assets with their
own cell size and per-tile properties are planned.

**BoxCollider**: `size [16, 16]` (positive), `offset [0, 0]`,
`isTrigger false`. Centred on `Transform.position + offset`. Solid colliders
on entities without a `KinematicBody` are static level geometry.

**KinematicBody**: no authored fields. Moved by gameplay code and stopped by
solids.

**PlatformerController**: `maxSpeed 110`, `acceleration 1200`,
`deceleration 1600`, `airAcceleration 900`, `jumpSpeed 300`, `gravity 900`,
`maxFallSpeed 420`, `jumpCutMultiplier 0.45` (0-1), `coyoteTime 0.08`,
`jumpBufferTime 0.1`. All non-negative. Requires `Transform`, `BoxCollider` and
`KinematicBody`. Reads the `move_left`, `move_right` and `jump` actions.

**Collectible**: `kind "coin"`, `value 1`. Collected when a `Player`'s
collider overlaps this entity's collider (normally a trigger).

**Player**: no fields. Marks the player character. Its start position is its
respawn point.

**Camera**: `follow` (entity id, optional), `zoom 1` (0-64, exclusive of 0),
`clampToBounds true` (keep the view inside the union of all tilemaps).

## Versioning

Each file type has its own `formatVersion`. The engine reads every version up
to its own and refuses newer ones with a message asking to update the engine.

When a format changes incompatibly:

1. Increment the version constant (`kSceneFormatVersion` or `kProjectFormatVersion`).
2. Add a migration function to the migration list in the format's source file.
   It upgrades the JSON from the previous version and runs before parsing, so
   the parser only ever sees the current version.
3. Add a test that loads a file in the old version.
4. Document the change here.

The engine always writes the current version. Writers emit every authored
field (not only non-default ones), so a later change of a default value never
changes the meaning of existing files.
