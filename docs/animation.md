# Animation guide

Characters and objects come alive with **animation clips**: frames from a
sprite sheet, played at a speed, looping or once. An **Animator** component
plays clips on the entity's Sprite and, on a character, picks *idle*, *run*,
*jump* and *fall* by itself from how the character moves. Scripts can play
any clip by name.

Status: **Milestone 5**. The Toolbox heroes and both examples' players are
animated this way.

## Making a clip in Studio

1. Import a sprite sheet (*Assets > Import...*): an image with the frames
   side by side, all the same size (for example 16x16 pixels each).
2. *Assets > + New > Animation...*: name it, choose the image and the frame
   size. The new clip starts with the frames of the image's first row and
   opens in the **animation editor**.
3. In the editor, the sheet is shown with a grid:
   - **click** a cell to add it as the next frame, **right-click** to take
     it out;
   - select a frame in the *Frames* strip to move it earlier or later, or
     remove it;
   - set **Frames/s** and **Loop**; the **Preview** plays the clip as it
     will look in the game.

   Changes are saved as you make them. Double-click an animation in Assets
   (or *Edit ...* under an Animator in Properties) to open it again.

## Animating a character

Select the character, *Add Component > Animator* (it needs a Sprite), and
pick clips:

| Field | Plays when |
|---|---|
| `idle` | the character stands still |
| `run` | it moves (on the ground, for a platformer) |
| `jump` | a platformer character goes up in the air |
| `fall` | a platformer character comes down |
| `clip` | nothing else applies: an entity without a controller (a torch, a flag), or a missing state clip |
| `speed` | 1 plays clips at their own speed, 2 twice as fast |
| `playing` | off holds the current frame |

Missing clips fall back sensibly: no `fall` uses `jump`, no `run` uses
`idle`. Dropping an animation from Assets onto an entity in the Scene view
adds an Animator with it (as `idle` on a character, as `clip` otherwise).
The Sprite's *Source* shows the first frame while editing; while playing,
the Animator sets the Sprite's texture and source every tick.

## From scripts

```lua
-- Plays a one-shot "Attack" clip when the player presses "attack",
-- then the Animator goes back to idle/run/jump/fall by itself.
local Fighter = {}

function Fighter:Tick()
    if Input.wasPressed("attack", self.entity) then
        self.entity:playAnimation("Attack")
    end
end

return Fighter
```

| Member | Use |
|---|---|
| `entity:playAnimation(name)` | plays the clip with this asset name from its start. A looping clip plays until another is chosen; a clip that does not loop plays once and then hands back to the automatic choice. |
| `entity:playAnimation(nil)` | back to the automatic choice at once |
| `entity.animation` | name of the clip playing now (`""` if none) |

Clips are found by their asset name, like prefabs: renaming a clip breaks
scripts that use the old name. Only authority scripts (single player, the
server) can play clips.

## Multiplayer

Animators run on the server; the frame they pick reaches every player as
part of the Sprite in the snapshots, so everyone sees the same animation.
Your own character's frame therefore follows the server's by about one
round trip while its position is predicted at once (good enough for walk
cycles; client-side animation comes with client scripts).

## File format

`animations/<name>.anim.json` (asset type `"animation"`):

```json
{
  "format": "px.animation",
  "formatVersion": 1,
  "texture": "<texture asset id>",
  "frames": [[32, 0, 16, 16], [48, 0, 16, 16], [64, 0, 16, 16], [80, 0, 16, 16]],
  "framesPerSecond": 10,
  "loop": true,
  "cellSize": [16, 16]
}
```

| Field | Required | Meaning |
|---|---|---|
| `texture` | yes | the sprite sheet (a texture asset) |
| `frames` | yes | texel rectangles `[x, y, w, h]` in playing order, at most 1024 |
| `framesPerSecond` | no (8) | greater than 0, at most 240 |
| `loop` | no (`true`) | `false`: play once and stay on the last frame |
| `cellSize` | no (`[16, 16]`) | the grid Studio's editor shows (not used when playing) |

The older `SpriteAnimation` component (frames stored in the scene) still
works; an Animator is the better choice for anything with more than one
animation.
