# Input scripts

Input scripts replay controller input tick by tick. They are used for
automated playthrough tests, headless runs, reproducible screenshots and bug
reports.

## Format

Plain text, one step per line:

```
<ticks> <action>[+<action>...]   hold these actions for <ticks> ticks
<ticks> -                        hold nothing for <ticks> ticks
```

`#` starts a comment; blank lines are ignored. Action names come from the
project's `input.actions`. Presses are derived automatically: an action
counts as pressed on the first tick it is held after not being held.

```
# run right, then jump while still running
60 move_right
20 move_right+jump
30 -
```

## Using them

```sh
# Headless, with a pass/fail check and a trace of the player every 10 ticks
px-headless examples/platformer --input-script examples/platformer/tests/full_run.input \
    --expect-all-collected --log-events --trace-every 10

# Visible, one tick per frame, saving the final frame
px-player examples/platformer --input-script examples/platformer/tests/full_run.input \
    --frames 400 --screenshot frame400.png
```

The simulation is deterministic, so a script produces the same result every
run on the same build. Changing level geometry or movement tuning can break a
script; re-check it with `--trace-every`.
