# Tilesets guide

A **tileset** says what the tiles of a tileset image are: which ones are
solid, which are **one-way platforms** you can jump up through, which are
**slopes**, which do not block at all, and which carry **tags** that scripts
can test ("water", "ice"). It belongs to the image, so every tilemap painted
with that image follows it.

Status: **Milestone 5**. The platformer tiles have two slopes and two
one-way planks (the example level has a slope at its first step and a plank
to jump onto); the top-down tiles make floors walkable and tag water.

## In Studio

1. Select a tilemap. In Properties, under *Paint tiles*, press **Edit Tile
   Properties** (or *Set Up Tile Properties* the first time; this creates
   `tilesets/<image>.tileset.json`).
2. In the tileset editor, click a tile (Ctrl+click adds more, Shift+click
   a range) and choose its **Collision**:

   | Collision | Marker | Behaves |
   |---|---|---|
   | Solid | none | blocks from every side (the default) |
   | None | cross | does not block: decoration, floors in a top-down game |
   | One-way | bar on top | a floor: jump up through it from below, walk through it sideways, land on it |
   | Slope / | diagonal | a 45-degree floor rising to the right |
   | Slope \ | diagonal | a 45-degree floor rising to the left |

   Add **tags** (up to 8 per tile). Changes are saved at once.
3. The markers also show in the tile palette and, while a tile tool is
   active, on the selected tilemap in the Scene view.

Collision only applies in tilemaps with **Solid** on; a tilemap with Solid
off is decoration (backgrounds).

### Tile tools

| Tool | Key | Use |
|---|---|---|
| Paint | B | drag to paint the chosen tile; right-drag erases |
| Rect | U | drag a rectangle to fill it; right-drag erases it |
| Fill | G | click to replace a tile and every same tile touching it (bucket fill); right-click erases them |
| Erase | E | drag to erase |

Alt+click picks the tile under the mouse. Every stroke, rectangle or fill is
one undo step.

## Slopes and one-way platforms in play

- A character walking onto a slope goes up it; walking down it stays on the
  ground (no hopping). The box rests on the slope with its lowest corner,
  so the high side of a slope should meet solid ground (or the next slope),
  as in the tile art.
- Slopes and one-way tiles are floors only: bodies pass through them from
  below and sideways.
- **One-way boxes**: a `BoxCollider` with *One-way* on is a platform you
  can jump up through. Moving platforms often want this.

## From scripts

```lua
-- Slows the player down while standing in water.
local Wading = {}

function Wading:Tick()
    local feet = self.entity.position + Vec2(0, 8)
    if World.tileHasTag(feet, "water") then
        self.entity.velocity *= 0.9
    end
end

return Wading
```

| Member | Use |
|---|---|
| `World.tileAt(position)` | the tile number at a world position (from the tilemap drawn on top), or nil |
| `World.tileHasTag(position, tag)` | true if a tile there has the tag |

## File format

`tilesets/<name>.tileset.json` (asset type `"tileset"`):

```json
{
  "format": "px.tileset",
  "formatVersion": 1,
  "texture": "<the tileset image's asset id>",
  "tileSize": [16, 16],
  "tiles": [
    { "tile": 8, "collision": "slopeUpRight" },
    { "tile": 10, "collision": "oneWay" },
    { "tile": 4, "collision": "solid", "tags": ["water"] }
  ]
}
```

Tiles are numbered left to right, top to bottom from 0, as in tilemaps.
Unlisted tiles are solid without tags. `collision` is `"solid"`, `"none"`,
`"oneWay"`, `"slopeUpRight"` or `"slopeUpLeft"`. A tilemap uses the tileset
whose `texture` is its image and whose `tileSize` matches its own.

## Limitations

- Slopes are 45 degrees only (one tile up per tile across); no gentle
  slopes or ceilings that slope yet.
- No dropping down through a one-way platform (down + jump) yet.
- Tiles cannot be changed from scripts yet (no `World.setTile`).
