# ADR 0017: Game types and a starter library copied into projects

- Status: Accepted
- Date: 2026-10-23

## Context

After the first Studio session the founder asked for two things: to choose
what kind of game to make (platformer, top-down, ...) when creating a
project, and to have plenty of ready-made assets available even in an
empty project, the way Roblox Studio's Toolbox offers models. Until then an
empty project had nothing in it, so a new creator had to import art and
write scripts before anything moved.

Questions to settle: where the kind of game lives and what it controls;
where ready-made content comes from; and how it gets into a project without
making projects depend on something outside their folder.

## Decision

1. **The game type is a project setting** (`settings.gameType`,
   `"platformer"` or `"topdown"`; project format 2 with a migration). It is
   an editor concern: it selects the default input actions, the player
   preset, the example to start from and the Toolbox items shown. The
   runtime does not branch on it; movement comes from the player's
   controller component (`PlatformerController` or the new
   `TopDownController`), so a scene can still mix anything.
2. **A starter library** (`library/`) holds images, behaviours and
   ready-made objects. Objects are `px.entities` documents, the same format
   as the clipboard, so inserting one is a paste. Its manifest
   (`px.library`) adds categories, descriptions, game types, tileset
   settings and icons ([library-format.md](../library-format.md)).
3. **Inserting copies files with the library's ids.** The files an item
   needs are copied into the project and registered under the library's
   asset GUIDs. Repeated inserts reuse them, the platformer example shares
   the same ids, and a project never refers to the library at run time:
   it plays in `px-player` and can be shared on its own.
4. **The library is generated** by `tools/generate_library.py` (art drawn
   by code, ids derived from names), like the examples, and validated when
   Studio loads it and in the editor tests (every object is inserted into
   empty projects of both types and played).

## Alternatives considered

- **Game type as a template only** (no stored setting): simplest, but Studio
  could not filter the Toolbox or pick the right player later, and the
  choice would be lost after creation.
- **Game type enforced by the runtime** (e.g. no gravity in top-down
  scenes): would stop creators mixing styles and duplicate what the
  controller components already express.
- **Projects referencing a shared library folder**: no copying, but a
  project would break when moved to another machine or published, and
  library updates would silently change existing games.
- **Prefab assets** instead of `px.entities` documents: the right long-term
  tool (Milestone 5), but prefabs need instancing and override semantics,
  and spawning must be designed with server authority (Milestone 4).
  Library objects become plain entities when inserted, so nothing has to
  change when prefabs arrive.
- **Downloading content from a server**: the platform has no backend yet
  (Milestone 6); a local library works offline and is versioned with the
  engine.

## Consequences

- An empty project is useful at once; a level can be built by clicking and
  dragging.
- Adding a kind of game costs a controller component, a `GameType` value,
  default input, a player object, art and an example.
- Copied files diverge from the library once edited; updating the library
  does not update existing projects (intended: games do not change under
  their creators).
- Creator-made Toolbox items and a marketplace (Milestone 8) can reuse the
  same manifest and insertion code with another source folder or a
  download step.
