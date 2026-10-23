# ADR 0016: Scene hierarchy: parent links in the scene, computed world transforms

- Status: Accepted
- Date: 2026-10-23

## Context
Creators need to group entities (a level's coins, a decorated platform that
moves as one, a character with attached parts) and Studio needs a tree in
its Hierarchy panel. ADR 0005 deferred the hierarchy to this milestone. The
choices affect the file format, scripts (what `position` means), physics,
rendering, copying and deletion, and later network replication.

## Decision
- **Parent links live in `Scene`**, not in a component: `m_parents[index]`,
  plus the scene's entity order kept in **depth-first pre-order** (each
  parent before its subtree, subtrees contiguous). The order is the
  Hierarchy panel's order, the file order and the update order.
- API: `createEntity(name, parent)`, `parent`, `children`, `subtree`,
  `isAncestor`, `depth`, `setParent(entity, parent, before,
  keepWorldTransform)` and a batched `setParents`. Re-parenting refuses
  cycles and depths above **64**. Destroying an entity destroys its subtree.
  `copySubtree` copies a subtree with new ids (duplicate, paste).
- **`Transform` is local to the parent.** World transforms are computed on
  demand (`worldTransform`, `worldPosition`, `setWorldPosition`) by
  composing ancestors' transforms (position, rotation, scale). Entities
  without a `Transform` (groups) do not affect their children. When rotation
  is zero the composition uses only additions and multiplications by scale,
  so positions stay exact and deterministic.
- Systems work in world space: physics, triggers, the camera and rendering
  use world positions; the platformer and kinematic movement convert their
  motion back to local positions (`moveWorld`). Scripts see world
  `position`/`rotation`, with `localPosition`/`localRotation`, `parent`,
  `getChildren()` and authority-only `setParent()` for the hierarchy.
- File format: entities carry an optional `"parent"` id; files list parents
  before children. This is scene format 3, with a migration from 2
  ([project-format.md](../project-format.md)).

## Alternatives considered
- **A `Parent`/`Children` component pair:** the common ECS approach, but
  parent/child consistency, ordering and cycle checks would be spread over
  component updates; the scene needs ordered children for the UI and files
  anyway.
- **Cached world transforms updated on change (dirty flags):** faster reads
  for deep hierarchies, but every write path (scripts, physics, editor,
  undo) must invalidate caches correctly. Scenes are shallow and small now;
  computing on demand is simple and always correct. Caching can be added
  behind the same API.
- **No transform inheritance (groups only for organisation):** simplest, but
  moving platforms with attached decoration, and character parts, are core
  2D needs.

## Consequences
- `worldTransform` costs O(depth) per call; with typical depths of 1-3 this is
  negligible, but systems that iterate thousands of deep entities per tick
  should be profiled before world transforms are cached.
- Colliders and tilemaps use world position but ignore rotation and scale
  (axis-aligned physics, ADR 0004); sprites honour all three.
- Moving a parent moves its children; physics moves the entity it acts on, so
  a body inside a moving parent is carried along, which is intended.
- Replication (Milestone 4) must send parent links and local transforms, and
  the order must stay depth-first for deterministic iteration.
