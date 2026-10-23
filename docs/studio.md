# Studio guide

Studio (`px-studio`) is the desktop editor. You create a project, build
scenes by placing and configuring entities, write Luau behaviours, and play
the game inside the editor. Everything Studio saves is the normal project
format ([project-format.md](project-format.md)), so `px-player` and
`px-headless` run it unchanged.

![Studio with the top-down example open: Toolbox on the left, Explorer and Properties on the right](images/studio.png)

Status: **foundation (Milestone 3)**. Verified on Linux (X11, OpenGL and
software renderer) and on a Windows 11 PC (Direct3D 11, built with Visual
Studio Build Tools 2026). Builds and passes its smoke test in CI on Windows
(MSVC) and macOS; not yet used on a real Mac. See [Limitations](#limitations).

## Starting Studio

```sh
./build/dev/apps/studio/px-studio                    # welcome screen
./build/dev/apps/studio/px-studio path/to/project    # open a project directly
```

| Option | Effect |
|---|---|
| `--reset-layout` | Forget the saved panel layout |
| `--verbose` | Debug messages in the Output panel |
| `--smoke-test` | Scripted run used by the automated tests (see [Testing Studio](#testing-studio)) |
| `--screenshot FILE` | With `--smoke-test`: save the final frame as a PNG |

The welcome screen creates a new project, opens a project folder, or reopens
a recent one. For a new project you give:

- a **name**, which is also the name of the project's folder;
- the **folder to create it in**. Studio remembers the folder you used last,
  so after the first project you only type a name. The first default is
  `Documents/Project Pixel Games` (created when needed);
- **what kind of game**: *Platformer* (side view with gravity: run and jump)
  or *Top-down* (seen from above: walk in every direction). This picks the
  player, the input actions (arrows and WASD, plus Space for jumping) and
  which Toolbox items are shown. It is stored in the project as
  `settings.gameType`;
- **what to start with**: a small example level of that kind (art, player,
  scripts, a finished level to play and change) or an empty scene with just
  a camera, ready for the Toolbox.

A project must go into a new or empty folder; Studio says so when a folder
with that name already exists and has files in it.

Settings (recent projects, the new-project folder, grid, snapping, UI scale,
layout) and the panel layout are stored per user: `~/.local/share/px/studio/` on Linux,
`%APPDATA%\px\studio\` on Windows, `~/Library/Application Support/px/studio/`
on macOS.

## The window

The standard layout puts the **Scene** view in the middle, **Explorer** above
**Properties** on the right, the **Toolbox** and **Assets** on the left (drag
things from there into the scene) and **Output** below the scene: the
arrangement many creators already know from other game-making tools.
*View > Layout > Classic* switches to Explorer on the left and Properties on
the right, with the Toolbox, Assets and Output below the scene.

| Panel | What it does |
|---|---|
| **Explorer** | The entities of the open scene as a tree. Drag an entity onto another to make it a child (hold Shift to place it before that entity instead); drop it on the empty area below to move it to the top level. Right-click for create, rename (F2), duplicate, copy, paste as child, delete. The search box filters by name. Entities with problems are shown in red. |
| **Scene** | The viewport. Select, move, paint tiles; shows colliders, camera bounds and the grid. While playing it shows the running game. |
| **Properties** | The selected entity's name, components and scripts. Every field of every component can be edited; hover a field's name for its description. Numbers are kept within their allowed range. |
| **Toolbox** | Ready-made things for every project, even an empty one: heroes, ground and walls, coins, gems, keys and locked blocks, spikes, enemies, springs and moving platforms, checkpoints, exits, signs, scenery, plus images, tilesets and behaviours. Click an item to put it in the middle of the view, or drag it to a spot in the Scene view; drop a behaviour on an entity. The files it needs are copied into the project the first time. Only items for the project's kind of game are shown unless *All game types* is ticked; the search box filters by name. |
| **Assets** | Images, scripts and scenes of the project. Import files, create scripts and scenes (*+ New*), rename, find uses, delete. Drag an asset onto a field, an entity or the Scene view. |
| **Output** | Log messages, script output and errors. Double-click an error with a file and line to open the script there. Repeated messages are folded. |
| **Script** | A plain text editor for `.luau` files, with save and compile errors. |
| **Project Settings** | Name, start scene, virtual resolution, background colour, input actions and keys (File > Project Settings). |

Panels can be dragged by their tab, docked anywhere and tabbed together.
*View > Layout > Reset Layout* puts every panel back; *View > UI Scale*
changes the size of everything (after a restart).

## Tutorial: a top-down game from the Toolbox

About five minutes, without importing or writing anything. (This sequence
was carried out in Studio to verify the Toolbox, and the game it makes was
played to the end.)

1. **New project.** On the welcome screen type a name, choose **Top-down**
   and **An empty scene**, then *Create Project*. You get a scene with a
   camera.
2. **Ground.** In the Toolbox, under *Ground*, click **Meadow** (a 20 x 12
   field of grass), then **Wall Ring** (solid walls around the same area).
3. **Hero.** Under *Characters*, click **Top-down Hero**. It appears in the
   middle and the camera follows it.
4. **Things to find.** Drag a **Gem** and a **Key** to two corners of the
   field, a **Lock Block** somewhere in between and a **Beetle** (it walks up
   and down and sends the hero back to the start) near the middle.
5. **The exit.** Under *Goals*, drag **Exit Stairs** to a corner.
6. **Save** with Ctrl+S, then **Play** with F5. Click the Scene view and
   walk with the arrow keys or WASD: the key opens the lock block, gems add
   to the score, the stairs complete the level. F5 again stops.

Everything placed is ordinary entities, components and scripts: select one
and change it in Properties (the beetle's *offset* and *period*, the
stairs' *message*), paint more walls with the Paint tool, or open a
behaviour in the Script panel to see how it works. The same steps make a
platformer: choose *Platformer*, then use *Ground*, *Platformer Hero*,
*Coin*, *Spikes*, *Spring* and *Goal Flag*.

## Tutorial: your own art and behaviour

Your own images and a script written from scratch, in about ten minutes.
(Carried out to verify Milestone 3; the resulting project then ran unchanged
in `px-player` and `px-headless`.)

1. **New project.** Choose *Platformer* and *An empty scene*.
2. **Import art.** In *Assets*, click *Import...* (or type a file path into the
   *or type a file path* field and press Enter, or drop files onto the window). For a
   quick start, import `tiles.png`, `player.png` and `coin.png` from
   `examples/platformer/assets`.
3. **Ground.** In Explorer, *+ Create > Tilemap*. It appears centred in the view with the
   imported tileset. Choose the Paint tool (B), pick a tile in the
   palette in Properties and drag along the bottom of the map; paint a short
   platform higher up. *Solid* is on, so the tiles block movement.
4. **Player.** *+ Create > Player*. It comes with a sprite, collider and
   platformer controller, and the camera starts following it.
5. **A coin.** Drag `coin.png` from Assets into the Scene view. The coin image
   is a strip of four frames: in Properties set *Source* to `0 0 16 16` and
   *Size* to `16 16`. *Add Component > BoxCollider* and tick *Trigger*.
6. **Its behaviour.** *Add Script*, type `Coin`, *Create*. The script opens;
   replace the template with:

   ```lua
   local Coin = {
       properties = { value = 5 },
   }

   function Coin:TriggerEnter(other)
       if not other:hasComponent("Player") then
           return
       end
       Game.addScore(self.value)
       Game.setObjective("Score: " .. Game.getScore())
       self.entity:destroy()
   end

   return Coin
   ```

   The line under the editor says *Compiles. Properties: value. Handles:
   TriggerEnter*, and Properties shows `value` for this coin.
7. **More coins.** Click the coin in the Scene view, press Ctrl+D and drag the
   copy somewhere else. Repeat.
8. **Save** with Ctrl+S, then **Play** with F5. Click the Scene view and use
   the arrow keys and Space. F5 again stops; everything is back as it was.
9. Run it outside Studio: `px-player "path/to/My Game"`.

## Building a level

1. **Create entities** from the Toolbox (ready-made, see above), with
   *+ Create* in Explorer or the Create menu, or by dragging an image from
   Assets into the Scene view (creates a sprite there). Presets: Empty, Group,
   Sprite, Tilemap, Solid Block, Trigger, Player, Camera. New entities appear
   in the centre of the view, on the grid, with a fitting image from the
   project if there is one. *Player* is the Toolbox hero for the project's
   kind of game; *Tilemap* in a project without a tileset uses the Toolbox's.
2. **Select** by clicking in the Scene view or in Explorer. Ctrl+click
   toggles, Shift+click adds, dragging on empty space selects with a box.
   Clicking the same spot again cycles through overlapping entities.
3. **Move** by dragging a selected entity, or with the arrow keys (1 unit;
   Shift+arrow moves one grid cell). With *Snap* on, drags move in whole grid
   steps; hold Alt to move freely. Escape cancels a drag. Exact values go in
   Properties (*Transform*).
4. **Organise** with Group entities: drag entities onto a group in
   Explorer. Children keep their place on screen when they are re-parented
   and move with their parent afterwards.
5. **Paint tiles**: select a Tilemap, pick a tile in its palette in Properties,
   choose the Paint tool (B) and drag in the Scene view. Right-drag or the
   Erase tool (E) erases; Alt+click picks the tile under the mouse. Resize the
   map in Properties (tiles are kept at the top left). Turn on *Solid* to
   make painted tiles block movement.
6. **Components**: *Add Component* in Properties, grouped by category.
   Adding *PlatformerController* or *TopDownController* also adds the
   components it needs.
   A component that another one needs cannot be removed until that one is.

### Viewport controls

| Input | Action |
|---|---|
| Mouse wheel | Zoom around the mouse |
| Middle drag, right drag, Space + left drag, Pan tool (H) | Pan |
| F | Frame the selection (or the whole scene) |
| Q / H / B / E | Select / Pan / Paint / Erase tool |
| Arrows, Shift+arrows | Nudge the selection |

The toolbar above the view has the tools, grid and snapping options, grid
size, collider display and zoom.

## Scripts

- *+ New > Script...* in Assets (or *Add Script > Create* in Properties)
  creates a script from a template with `Start`/`Tick`/trigger handlers and a
  `properties` table.
- Attach scripts by dragging them from Assets onto an entity (in the Scene
  view or in Explorer) or onto the *Add Script* button in Properties, or pick
  one from *Add Script*, which also lists the Toolbox's behaviours (patrol,
  hazard, spring, key, checkpoint and more) under *From the Toolbox*. An
  entity can have up to 16 behaviours; they run in the order listed.
- Each attached script shows the properties it declares, with their types and
  defaults. Values you change are saved with the entity; *Reset* returns a
  property to the script's default.
- Edit scripts in the Script panel (double-click a script asset) or in your
  own editor. Studio recompiles a script when it is saved and notices when a
  file changes on disk. Compile errors show in Properties, Output and the
  Script panel.

Scripting itself is described in [scripting.md](scripting.md) and
[scripting-api.md](scripting-api.md).

## Playing

| Button | Key | Effect |
|---|---|---|
| Play | F5 | Runs the open scene as it is now, unsaved scene and script changes included |
| Pause | F6 | Freezes the game; Properties shows its live state |
| Step | F7 | Advances one tick (1/60 s) while paused |
| Stop | F5 | Returns to editing |

The game runs on a **copy** of the scene, with the same simulation as
`px-player`. Changes made while playing (moving things, editing fields in
Properties) affect only the running game and are discarded on Stop; the
orange *PLAYING* label in the toolbar and the Scene view shows that you are
playing. Click the Scene
view to give the game keyboard input. *Game camera* switches between what
the player sees and a free editor camera. Script errors appear in Output
and stop only the failing behaviour, as in `px-player`.

## Saving

- **Ctrl+S** (File > Save All) saves the scene, all changed scripts and the
  project settings. An asterisk in the window title (and in a script's tab)
  marks unsaved changes.
- Studio refuses to save a scene that would not load (for example a sprite
  without an image); such entities are red in Explorer, and
  Properties lists the problems.
- Closing the project, opening another scene or quitting with unsaved
  changes asks whether to save.
- Files are written in a canonical form (stable order, two-space JSON), so
  saving an unchanged scene produces no difference in version control.
  `px-headless --resave PROJECT` rewrites a hand-edited project the same way.

## Undo

Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z). Every operation is one step: a whole drag,
a typed value, a paint stroke, a paste. The Edit menu shows what will be
undone. Undo history is per scene, is kept until the scene is closed, and is
bounded in memory (oldest steps are dropped). Script text has its own undo
in the Script panel. Asset operations (import, rename, delete) and project
settings are not undoable; deleting an asset asks first and lists where it
is used.

## Copy, paste and duplicate

Ctrl+C copies the selected entities, with their children, as text (a
`px.entities` JSON document) to the system clipboard; Ctrl+V pastes them into
the open scene. Pasting into another project works when it has the same
assets (for example two projects made from the same template); otherwise
Studio names the missing asset and pastes nothing. Ctrl+D duplicates in place.
References between copied entities (a camera following a copied player) point
to the copies; references to entities outside the copy are kept when they
exist in the target scene.

## Keyboard shortcuts

| Keys | Action |
|---|---|
| Ctrl+S | Save all |
| Ctrl+O | Open project |
| Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z | Undo, redo |
| Ctrl+C, Ctrl+V, Ctrl+D, Delete | Copy, paste, duplicate, delete (when Explorer or the Scene view has focus) |
| Ctrl+A | Select all top-level entities (moving them moves everything) |
| F2 | Rename (Explorer) |
| F5, F6, F7 | Play/Stop, Pause, Step |

## Testing Studio

`ctest` runs `studio_smoke_headless_display`: Studio opens the example
project with SDL's dummy video driver and software renderer, then, through
the same code paths as the UI, selects the player, opens a script and the
project settings, creates every entity preset, inserts the Toolbox objects
whose files the project already has, duplicates, copies and pastes,
switches to the paint tool, undoes everything (the scene must be exactly as
saved), plays for two seconds holding *right* (the player must collect a
coin without script errors) and stops (the scene must be unchanged). It
writes nothing to the project. `--screenshot` saves the final frame.

The editor model (`engine/editor`) has its own unit tests
(`tests/test_editor.cpp`): project creation, assets, saving and reloading,
undo/redo, hierarchy operations, copy/paste, picking and play mode, and the
starter library: every Toolbox object is inserted into empty projects of
both kinds, saved, reloaded and played without script errors.

## Limitations

These are known and planned, not bugs:

- **No prefabs** yet: copy/paste and duplicate are the way to reuse entities.
- The Toolbox has the built-in starter library only; creators cannot add
  their own items to it yet. Two kinds of game so far (platformer and
  top-down); more are planned and need only a controller, art and a library
  entry each.
- **One entity at a time in Properties**; with several selected it shows the
  last one clicked (moving, duplicating and deleting work on all).
- The Scene view moves entities; rotation and scale are typed or dragged in
  Properties (no rotate/scale handles yet).
- Undo keeps whole-scene snapshots: fine for scenes of thousands of entities,
  but memory-bound for very large ones (the oldest steps are dropped).
- The script editor is plain text: no syntax highlighting or completion.
- Rotation and scale affect sprites, but colliders and tilemaps ignore them
  (as in the runtime).
- No multi-scene editing, no scene transitions from scripts yet.
- Studio has been used by its developers and the founder only, and not yet
  on a real Mac.
- Studio is not available in the browser build.

Design and rationale: [ADR 0013](adr/0013-studio-ui-dear-imgui.md) (UI
toolkit), [ADR 0015](adr/0015-studio-architecture.md) (editor architecture
and undo).
