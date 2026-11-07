# Input devices and player settings

Games use **actions** (`move_left`, `jump`, …) and give each one keyboard keys
in `project.json` (see [project-format.md](project-format.md)). The player
turns every input device into those same actions, so a game made for the
keyboard is also playable with a gamepad and on a touch screen, with no work by
its creator. All devices can be used at the same time.

## Keyboard

The keys given in the project. In addition, outside the game:

| Key | Does |
|---|---|
| Esc | the menu (settings, quit) |
| R | restart the level (alone) |
| F5 | reload scripts and scenes from disk |
| F1 | debug overlay |
| F2 | screenshot |

## Gamepads

Any gamepad SDL knows (Xbox, PlayStation, Switch Pro, most others) works as
soon as it is connected. Each button stands for the keys a player would expect
it to:

| Gamepad | Keys it stands for |
|---|---|
| D-pad and left stick | arrows and W A S D |
| South (A / cross) | Space, Z, J |
| West (X / square) | X, K |
| East (B / circle) | C, E, L |
| North (Y / triangle) | V, Q, I |
| Start | Enter (and opens the menu) |
| Back / Select | Tab, Backspace |
| Left shoulder | Shift |
| Right shoulder | Ctrl |

A button presses every action bound to any of its keys
(`runtime::padButtonForKey`). The left stick works in eight 45° sectors past
43 % of its travel, so running right with the stick a little high does not
also press up (which jumps in many side-view games).

An action bound only to other keys (F keys, digits, P, …) is not reachable
with a gamepad. Creators who want pad support use keys from the table.

In the menu: the D-pad moves and changes values, South chooses, East or Back
closes it.

## Touch screens

On touch laptops the player draws on-screen controls when the screen is
first touched, on phones and tablets from the start (setting **Touch
controls: Auto**):

- a direction pad, bottom left: a cross when the game has `move_up` or
  `move_down`, two arrows otherwise. A thumb between two arrows presses both
  (diagonals), because the pad goes by the direction from its centre;
- one button for each other action, bottom right, at most four; the first
  (usually jump) is the largest. Labels are the first five letters of the
  action's name;
- a pause button, top right, which opens the menu. Tap a row of the menu to
  choose it.

Several fingers work at once. Touches slightly outside a button still count.
The layout comes from `runtime::TouchControls` and is in the game's virtual
pixels, so it scales with the game.

To try the touch controls with a mouse: `px-player --touch` (the mouse acts
as one finger).

## Settings

The menu (Esc, Start, the pause button or Android's back button) holds the
settings. They belong to
the device, not the game, and are saved at once, in the player's preferences
folder (`settings.txt`, for example `%APPDATA%\ProjectPixel\player` on
Windows and `~/.local/share/ProjectPixel/player` on Linux):

| Setting | Values | |
|---|---|---|
| Volume | 0-100% (80%) | all sound; "no sound" when the device has none ([audio.md](audio.md)) |
| Music | 0-100% | the games' music |
| Effects | 0-100% | everything else |
| VSync | on, off | off can tear but has less delay |
| Frame limit | none, 30, 60, 120 | saves battery; not in browsers, which pace frames themselves |
| Scaling | game default, pixel perfect, fit screen | pixel perfect keeps every pixel the same size, with borders; fit screen fills more of it |
| Show FPS | on, off | frames per second, top right |
| Touch controls | auto, on, off | auto: shown once the screen is touched |

The game keeps running at its fixed tick rate whatever the frame rate. Playing
alone, the menu pauses the game; in multiplayer the game goes on.

Runs with `--frames` (tests and screenshots) ignore saved settings, so their
results do not depend on the computer. `--touch` turns the touch controls on
for that run only.

In the browser the settings last until the tab is closed.
