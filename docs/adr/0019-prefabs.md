# ADR 0019: Prefabs: copies tracked by derived ids, updated by three-way merge

- Status: Accepted
- Date: 2026-11-02

## Context

Milestone 5 adds prefabs: entity trees saved as assets, placed as copies in
scenes and spawned by scripts. Requirements:

- Changing a prefab must update its copies, while each copy keeps what was
  changed on it (position at least; in practice also values like a coin's
  points or a bigger collider).
- Copies must survive copy and paste, duplicate, undo, hand-edited files and
  older files, and references from other entities into a copy (a camera
  following a copy's child) must keep working across updates.
- Spawning at run time must be deterministic (tests, replays, the server).
- Scenes must stay readable JSON that loads without the prefab files (the
  player, the server and clients load scenes as they are).

Common designs: (a) Unity-style: a copy is stored as a link plus a list of
property overrides, and resolved when the scene loads; (b) Roblox-style
packages: a copy is a full copy of the content plus a link, replaced
wholesale on update ("Update All"); (c) copies are plain entities and
nothing is tracked.

## Decision

1. **Copies are stored in full** in the scene, with a `PrefabInstance`
   component on the root naming the prefab. Scenes load and run without
   prefab files; prefabs matter only to Studio and to `World.spawn`.
2. **Ids of a copy's entities are derived**: prefab entity id XOR the
   copy's key, where key = root id XOR prefab root id. The mapping between a
   copy's entities and the prefab's is therefore arithmetic, needs no stored
   table, and nested copies stay consistent (XOR composes). Copying a copy
   (paste, duplicate) re-keys it: every id of the copy is XORed with
   (old root id XOR new root id) instead of getting a random id.
3. **Updates are a three-way merge** of the copy, the prefab's previous
   content and its new content. A value (per component field, per script
   property, plus names and component presence) that differs between the
   copy and the previous content is the copy's own and is kept; everything
   else comes from the new content. Entities added to a copy are kept under
   the same parent; prefab entities deleted from a copy stay deleted.
   Nothing about overrides is stored, so nothing can drift out of sync.
4. **"Apply to Prefab" updates every copy at once**: in the open scene as
   one undo step, and in the project's other scene and prefab files, which
   are rewritten immediately. There is no lazy refresh on load.
5. **Spawning** copies the loaded prefab with ids derived from a per-session
   counter, so the same session spawns the same ids. Script instances of the
   new entities are created after the current phase (the VM refuses nested
   protected calls) and start at the next tick. Limits: 1000 entities per
   tick, 100 000 per scene.

## Consequences

- Copies cost their full size in scene files, like any entities; diffs of
  scene files show every copy changing when a prefab changes. Acceptable for
  the scene sizes of this stage; a compact form can be added later as a
  format change without changing the model.
- The previous prefab content is needed to merge, so updates happen when
  the prefab is applied in Studio. A prefab file edited by hand (or replaced)
  does not update existing copies until it is applied from a copy; Revert
  brings a single copy in line.
- Scripts refer to prefabs by name (`World.spawn("Bullet")`), like Roblox
  scripts refer to objects by name. Renaming a prefab breaks such calls;
  Studio does not find them yet.
- Multiplayer needs no new protocol: spawned entities are new entities in
  the next snapshots, sent in full once.

## Alternatives considered

- **Overrides list (Unity)**: compact files, but the override list must be
  kept correct through every editing operation, and scenes would not load
  without their prefabs. More machinery than this stage needs.
- **Replace on update (Roblox packages)**: simple, but loses every per-copy
  change except the root's placement, which makes prefabs awkward for
  things like coins with different values.
- **Stable mapping table per copy**: works, but must be maintained through
  paste, duplicate and hand edits; derived ids need none.
