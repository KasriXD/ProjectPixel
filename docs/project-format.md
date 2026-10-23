# Project format

Current versions: project manifest **2**, scene **3**. Starter library
manifests and `px.entities` objects are described in
[library-format.md](library-format.md).

A game project is a folder:

```
my-game/
  project.json            manifest (required, this exact name)
  scenes/*.scene.json     scenes
  scripts/*.luau          Luau scripts (docs/scripting.md)
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
  "formatVersion": 2,
  "id": "af5d3a02-6054-58d1-a81c-23a97281ae0f",
  "name": "Example Platformer",
  "engineVersion": "0.3.0",
  "startScene": "e0464a98-2756-52ff-8acd-37c81877ff1a",
  "settings": {
    "gameType": "platformer",
    "tickRate": 60,
    "display": { "virtualResolution": [320, 180], "scaling": "integer", "windowScale": 3 },
    "scripting": { "apiVersion": 1 }
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
    { "id": "e0464a98-...", "type": "scene", "name": "Level 1", "path": "scenes/level1.scene.json" },
    { "id": "4be1b3f2-...", "type": "script", "name": "Coin", "path": "scripts/coin.luau" }
  ]
}
```

| Field | Required | Meaning |
|---|---|---|
| `format`, `formatVersion` | yes | `"px.project"`, `2` |
| `id` | yes | project GUID |
| `name` | yes | display name, not empty |
| `engineVersion` | no | engine version that last saved the file (informational) |
| `startScene` | yes | GUID of a `scene` asset |
| `settings.gameType` | no (`"platformer"`) | the kind of game: `"platformer"` (side view with gravity) or `"topdown"` (seen from above). Studio uses it to choose the player preset, default input actions and which Toolbox items to show; the runtime does not depend on it (movement comes from the player's controller component) |
| `settings.tickRate` | no (60) | fixed simulation steps per second, 1-240 |
| `settings.display.virtualResolution` | no ([320, 180]) | logical screen size in pixels |
| `settings.display.scaling` | no (`"integer"`) | `"integer"` (whole-number scale, crisp pixels) or `"letterbox"` |
| `settings.display.windowScale` | no (3) | initial desktop window size multiplier, 1-16 |
| `settings.scripting.apiVersion` | no (1) | scripting API version the project was written for; newer than the engine's is refused |
| `input.actions` | no | action name → list of default keys. At most 32 actions; names use `a-z`, `0-9`, `_`. Order defines action ids. |
| `assets` | no | asset list, see below; at most 10 000 entries |

Key names: `A`-`Z`, `0`-`9`, `Space`, `Enter`, `Escape`, `Backspace`, `Tab`,
`Left`, `Right`, `Up`, `Down`, `LeftShift`, `RightShift`, `LeftCtrl`,
`RightCtrl`, `LeftAlt`, `RightAlt`, `F1`-`F12` (case-insensitive). Keys are
physical positions, so `W`/`A`/`S`/`D` stay in place on non-QWERTY layouts.

Standard actions used by built-in components: `move_left`, `move_right` and
`jump` (`PlatformerController`); `move_left`, `move_right`, `move_up` and
`move_down` (`TopDownController`). New projects get the set for their game
type, each on the arrow keys and WASD (plus Space and Z for jumping).

### Assets

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | asset GUID, unique |
| `type` | yes | `"texture"`, `"scene"` or `"script"` |
| `path` | yes | file path inside the project |
| `name` | no | label for tools (defaults to the path) |
| `filter` | no (`"nearest"`) | textures only: `"nearest"` for pixel art, `"linear"` for smooth art |

Textures may be PNG, JPEG, BMP or TGA, at most 8192x8192 pixels.
Scripts are Luau source files (UTF-8, at most 1 MiB). All scripts are
compiled when the project loads; a syntax error stops loading and names the
file and line.

## Scene files

```json
{
  "format": "px.scene",
  "formatVersion": 3,
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
    },
    {
      "id": "0a7d41a6-...",
      "name": "Coin 1",
      "parent": "c3b42f97-...",
      "components": {
        "Transform": { "position": [104, 120] },
        "Tags": ["coin"],
        "Scripts": [ { "script": "4be1b3f2-...", "properties": { "value": 1 } } ]
      }
    }
  ]
}
```

| Field | Required | Meaning |
|---|---|---|
| `name` | no | scene display name |
| `settings.backgroundColor` | no | clear colour, `"#rrggbb"` or `"#rrggbbaa"` |
| `settings.killPlaneY` | no | players whose Y exceeds this are respawned at their spawn point (platformers; top-down scenes have none) |
| `entities` | yes | at most 100 000 entities, see below |

Each entity has:

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | GUID, unique in the scene |
| `name` | no | display name; need not be unique |
| `parent` | no | id of the parent entity. Its `Transform` is relative to the parent's. Hierarchies may be at most 64 levels deep and may not loop. Destroying an entity destroys its children. |
| `components` | no | object mapping component names to their data |

Studio writes entities in hierarchy order (each parent before its children,
depth first), which is also the draw order within a layer and the order in
which scripts run. The loader accepts any order and restores it.

Units: world units, where 1 unit is 1 pixel at zoom 1. Y points **down**.
Angles are degrees, clockwise. Vectors are `[x, y]`; rectangles are
`[x, y, width, height]` with `(x, y)` the top-left corner.

### Components

Every component, field, default and allowed range is listed in the generated
**[component reference](components.md)**, produced from the same descriptor
table that the loader, the writer and Studio's Properties panel use. In short:

| Component | Purpose |
|---|---|
| `Transform` | position, rotation, scale relative to the parent |
| `Sprite`, `SpriteAnimation` | draw an image; play frames on it |
| `Tilemap` | grid of tiles from a tileset; `rows` are strings of space-separated tile numbers or `.` for empty |
| `BoxCollider` | solid box or trigger |
| `KinematicBody` | moved by velocity, stopped by solids |
| `PlatformerController` | running and jumping; requires `Transform`, `BoxCollider`, `KinematicBody` |
| `TopDownController` | walking in eight directions without gravity, seen from above; requires the same |
| `Player` | the player character; respawns at its spawn point (its start position unless a script moves it) |
| `Camera` | the view; can follow an entity |
| `Tags` | an array of labels, e.g. `["coin"]` |
| `Scripts` | an array of Luau behaviours, each `{ "script": <script asset id>, "properties": {...} }` |

Script properties override defaults declared in the script's `properties`
table and must have the same type: number, `true`/`false`, string, or vector
`[x, y]`. Undeclared names and wrong types are load errors. Up to 16 scripts
per entity; each runs as its own instance (see [scripting.md](scripting.md)).

Physics and rendering use world positions (the composed transform of the
entity and its ancestors). Colliders and tilemaps are axis-aligned: rotation
and scale do not apply to them. Solid `BoxCollider`s without a
`KinematicBody` may be moved by scripts (moving platforms); bodies standing
on them are carried along.

### Copied entities

Studio copies entities to the clipboard as a `px.entities` document: the same
entity objects as a scene, plus `"sceneFormatVersion"`. Pasting gives every
entity a new id and rewires references between the pasted entities.

## Format history

| Scene format | Engine | Change | Migration |
|---|---|---|---|
| 1 | 0.1 | Initial format | |
| 2 | 0.2 | Added `Script` and `Tags`. Removed `Collectible`: collecting is now scripted | `Collectible { kind }` becomes a `kind` tag; `value` is dropped. A warning tells the creator to add a script (see the example's `coin.luau`) |
| 3 | 0.3 | Entity `parent` (hierarchy). `Script` replaced by `Scripts`, a list, so an entity can have several behaviours | `"Script": {...}` becomes `"Scripts": [{...}]`; behaviour is unchanged |

| Project format | Engine | Change | Migration |
|---|---|---|---|
| 1 | 0.1 | Initial format; `script` assets and `settings.scripting` were added compatibly in 0.2 | |
| 2 | 0.3 | Added `settings.gameType` | Projects of format 1 become `"platformer"` (every earlier project was one) |

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
