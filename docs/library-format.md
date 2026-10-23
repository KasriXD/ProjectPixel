# Starter library format

The starter library is the content behind Studio's **Toolbox**: images,
behaviours (Luau scripts) and ready-made objects that can be put into any
project, including an empty one. It lives in `library/` in the source tree
and next to `px-studio` in a package.

```
library/
  library.json              manifest (this exact name)
  art/*.png                 images and tilesets
  scripts/*.luau            behaviours
  objects/*.entities.json   ready-made objects (px.entities documents)
```

The library is generated: `tools/generate_library.py` draws the art (with
`tools/library_art.py`), copies the platformer example's art and scripts,
writes the objects and the manifest. Change the generator, not the files,
then run it (it needs Python 3 and Pillow) and commit the result. See
[CONTRIBUTING.md](../CONTRIBUTING.md#starter-library).

## library.json

```json
{
  "format": "px.library",
  "formatVersion": 1,
  "name": "Starter Library",
  "assets": [
    {
      "id": "782c3432-d57a-558a-a29f-2eb189772494",
      "type": "texture",
      "name": "Platformer Tiles",
      "path": "art/tiles.png",
      "filter": "nearest",
      "category": "Tiles",
      "gameTypes": ["platformer"],
      "tileSize": [16, 16],
      "tilesetColumns": 4
    },
    { "id": "...", "type": "script", "name": "Coin", "path": "scripts/coin.luau",
      "category": "Behaviours", "description": "Collect a coin: adds points, then disappears" }
  ],
  "objects": [
    {
      "id": "fb02102e-6f8b-5471-aa06-8567536e3588",
      "name": "Platformer Hero",
      "category": "Characters",
      "gameTypes": ["platformer"],
      "description": "The player: runs and jumps with the arrow keys and Space.",
      "path": "objects/platformer_player.entities.json",
      "icon": { "texture": "2e723cc5-e1d9-5ea1-8b39-72e1274823fd" }
    }
  ]
}
```

### Assets

The same fields as a project's asset list ([project-format.md](project-format.md#assets)),
plus:

| Field | Required | Meaning |
|---|---|---|
| `category` | no | Toolbox heading |
| `description` | no | tooltip text |
| `gameTypes` | no (all) | `"platformer"`, `"topdown"`; the Toolbox shows only items for the project's game type unless *All game types* is ticked |
| `tileSize`, `tilesetColumns` | no | textures only: marks a tileset. Inserting it creates a Tilemap with these settings; any other image becomes a sprite of its own size |

Only `texture` and `script` assets are allowed (not scenes). Ids must be
unique; paths follow the project rules (relative, no `..`).

### Objects

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | GUID, unique among the objects |
| `name` | yes | tile label, not empty |
| `category`, `description`, `gameTypes` | no | as for assets |
| `path` | yes | a `px.entities` document (the clipboard format of [project-format.md](project-format.md#copied-entities)) |
| `icon.texture`, `icon.source` | no | the Toolbox picture: a texture of this library and an optional `[x, y, w, h]` part of it |

An object may contain several entities with a hierarchy. The first top-level
entity with a Transform is the anchor; the other top-level entities keep their
offset from it. Studio places the anchor where the object is dropped (in the
centre of the view on a click); tilemaps are centred on that point and
snapped to their tile size. Objects may refer only to assets of the library.

## Loading and validation

Studio loads the library at start-up (next to the executable first, then the
source tree). Every file must exist, every image must decode, and every object
is test-pasted into an empty scene with the library's assets, so a broken
library is reported once, in Output, and not when someone drags an item. If
the library cannot be loaded, the Toolbox says why and the rest of Studio works
normally.

## Inserting into a project

Inserting an item copies the files it needs into the project's `assets/` or
`scripts/` folder and adds them to `project.json` **with the library's ids**.
An asset the project already has (same id) is reused, so inserting ten coins
adds one image and one script; projects created from the examples already
share those ids. A file name or asset name already used gets a number
(`gem-2.png`, *Gem 2*). The project then no longer depends on the library:
it runs in `px-player` and can be shared like any other project.

Pasting the object's entities, moving them into place and following a new
player with an idle camera are one undo step. The copied files stay in the
project after undo (like an import); delete them in Assets if they are unused.
