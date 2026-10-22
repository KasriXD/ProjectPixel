# ADR 0004: Own kinematic collision first, Box2D later

- Status: Accepted
- Date: 2026-10-22

## Context
Milestone 1 needs reliable collision with solid platforms and tiles,
triggers and responsive character movement. Later, some games will want
rigid bodies (crates, ragdolls, vehicles). Multiplayer benefits from
deterministic simulation.

## Decision
- The `physics` module provides a small **kinematic collision world**: static
  solid grids and boxes, and swept, axis-separated AABB movement with contact
  flags. Character controllers are written on top of it.
- **Box2D v3 (MIT, C)** is the planned rigid-body engine, added behind the
  `physics` module when a game needs dynamic bodies. Gameplay code and the
  rest of the engine will keep talking to `px::physics` types only.

## Alternatives considered
- **Box2D for everything now:** platformer characters built from rigid
  bodies feel floaty and need workarounds (friction hacks, ground probes),
  and it would be a dependency without a current need. (Box2D 3.1+ has a
  character "mover" API worth revisiting when integrating.)
- **Chipmunk2D:** less actively maintained.

## Consequences
- Tile-based platformer movement is exact and deterministic, with no ghost
  collisions at tile seams, and is covered by unit tests.
- Static geometry is built once per session; moving solid platforms need a
  dynamic-solids extension (planned with Milestone 2's scripting examples).
- Slopes and one-way platforms are not supported yet.
