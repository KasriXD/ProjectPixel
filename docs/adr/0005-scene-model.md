# ADR 0005: Scene model: own ECS with GUID identity

- Status: Accepted
- Date: 2026-10-22

## Context
The engine must own its scene and component model (it is the creator-facing
heart of the product). It must serialize cleanly, support stable references
for scripting and networking, copy cheaply for play-mode, and stay fast.

## Decision
- Entities are **generational handles** in a `Registry`; components are plain
  structs stored per type in **sparse sets** (dense arrays + index map).
- Every entity has an `EntityInfo` with a **GUID** and a name. GUIDs are the
  persistent identity used by files, cross-entity references, and later
  scripts and network replication. Handles are never persisted.
- `Scene` keeps creation order (stable saving, future hierarchy panel) and is
  deep-copyable.
- Behaviour lives in systems (functions over component sets), not in
  components.

## Alternatives considered
- **EnTT:** excellent and MIT-licensed, but the scene model is core product
  surface we want to shape ourselves (reflection for Studio, scripting
  bindings, replication), and our needs so far fit in ~200 lines.
- **Object hierarchy with virtual methods (classic OOP):** familiar to
  creators, but harder to serialize generically, to replicate and to keep
  cache-friendly.

## Consequences
- Simple, tested storage. Component type ids rely on static linking
  (`detail::componentTypeId`); engine plugins as shared libraries would need
  a registration-based id scheme.
- Parent/child hierarchy: implemented in Milestone 3 as parent links in the
  `Scene` with computed world transforms ([ADR 0016](0016-scene-hierarchy.md)).
- Component reflection: implemented in Milestone 3 as component descriptors
  shared by the file format, validation, Studio and the reference docs
  ([ADR 0014](0014-component-descriptors.md)).
- Deep copies are what Studio's undo and play mode are built on
  ([ADR 0015](0015-studio-architecture.md)).
