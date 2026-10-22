# ADR 0007: Fixed-tick simulation driven by input frames, separate from presentation

- Status: Accepted
- Date: 2026-10-22

## Context
Multiplayer is a foundational capability: a dedicated server must run games
without graphics, and the server must be authoritative. Automated testing of
gameplay needs reproducible runs.

## Decision
- All gameplay lives in `runtime::GameSession`, which has no graphics,
  window or audio dependency (enforced by CMake target dependencies).
- The session advances only through `tick(InputFrame)`: fixed steps of
  `1 / tickRate` seconds. No gameplay code reads the wall clock.
- Input reaches gameplay only as **named actions** in `InputFrame`s
  (held and pressed bitmasks). Device mapping happens in the client.
- Rendering reads the scene after ticks and never modifies it.
- `px-headless` runs sessions without graphics; it becomes the dedicated
  server in Milestone 4.

## Alternatives considered
- **Variable timestep:** simpler at first, but non-deterministic and
  frame-rate dependent (jump heights change with FPS).
- **Gameplay reading devices directly:** cannot be sent over a network or
  replayed.

## Consequences
- Identical input produces bit-identical results on the same build (tested),
  enabling scripted playthrough tests, replays and server reconciliation.
- Cross-platform bit-exact determinism (needed only for lockstep networking)
  is **not** guaranteed: floating-point behaviour can differ between
  compilers. The planned networking model is server-authoritative snapshots,
  which does not require it.
- Rendering can show up to one tick of latency; render interpolation
  (`FixedStepClock::alpha`) is available when needed.
