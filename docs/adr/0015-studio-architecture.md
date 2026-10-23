# ADR 0015: Studio architecture: a UI-free editor model with snapshot undo

- Status: Accepted
- Date: 2026-10-23

## Context
Studio must create and open projects, edit scenes with undo, manage assets,
edit scripts, and play the game, without corrupting projects and without
tying the editing logic to one UI toolkit ([ADR 0013](0013-studio-ui-dear-imgui.md)).
Undo must cover every kind of change (fields, hierarchy, tiles, scripts
attached, paste), be impossible to forget for new operations, and group a
whole drag into one step. Play must never change the edited scene.

## Decision

**Module.** `engine/editor` (`px_editor`) depends on `project`, `runtime`,
`scripting` and `scene`, never on SDL or ImGui. It has four parts:

- `EditorProject`: an open project folder. Creating (empty or from a
  template), the manifest and settings, importing, creating, renaming and
  deleting assets (deletion is refused while an asset is used; `findUses`
  lists where), loading and saving scenes, unsaved script buffers with
  incremental recompilation and detection of changes made by other programs,
  and building the script library for play.
- `SceneDocument`: one open scene, its selection and its undo history. All
  operations (create from presets, rename, delete, duplicate, copy/paste,
  re-parent, add/remove components, attach scripts, set script properties,
  move, resize and paint tilemaps) are methods here.
- `Picking`: hit testing, box selection, tile coordinates and grid snapping,
  in world space.
- `PlayMode`: runs a `GameSession` on a **copy** of the document's scene
  with the current (possibly unsaved) script buffers; pause and single-step.
  Stopping discards the copy. The same `GameSession` as `px-player` runs, so
  what plays in Studio is what ships.

**Undo by snapshots.** Every change happens inside an edit transaction:
`beginEdit(label)` stores a copy of the scene (scenes are value types,
[ADR 0005](0005-scene-model.md)), the change is applied, `markChanged()`
records that something changed, and `endEdit()` pushes one undo step, or
nothing if nothing changed. Transactions nest, so operations compose (create
a sprite, then size it, as one step). Studio opens a transaction when a
widget becomes active and closes it when it is released, so a drag or a
typed word is one step; Escape cancels a drag by restoring the snapshot.
History is bounded by memory (256 MiB estimate) and 500 steps; the oldest
steps are dropped first.

**Selection by id.** The selection is stored as entity GUIDs, so it survives
undo, redo and reloads; handles are resolved when needed.

**Dirty tracking by version.** Each committed change and each undo/redo
moves the document to a new version; the document is dirty when the version
differs from the one saved. Undoing back to the saved state makes it clean
again.

**Safe saving.** A scene is validated with the same rules as the loader
(component descriptors, [ADR 0014](0014-component-descriptors.md)) and
refused if it would not load. Files are written in a canonical form, so
saving an unchanged scene changes no bytes (tested on the example project,
which is stored exactly as Studio saves it). The manifest is re-parsed
before writing. Paths stay inside the project folder (`DirectoryFileSystem`).

## Alternatives considered
- **Command objects with do/undo per operation:** the classic design,
  memory-efficient, but every operation needs a correct inverse, including
  hierarchy changes, deletes of subtrees with references, and paste. Each
  new feature is a new chance of an undo bug. Snapshots make undo correct by
  construction.
- **Snapshots of serialized JSON:** smaller, but serializing and parsing on
  every step costs far more than copying the in-memory scene, and loses
  runtime-only state.
- **Diff-based snapshots (store only changed components):** best of both,
  but more complex; it is the planned evolution if memory becomes a problem
  (the `SceneDocument` API does not change).
- **Editing logic inside the UI code:** fastest to start, but untestable
  without a window and lost when the UI is replaced.

## Consequences
- Measured (Release, Linux): a snapshot of a 1,000-entity scene costs
  0.16 ms and ~0.4 MB; of a 10,000-entity scene 1.7 ms and ~3.7 MB, so the
  budget keeps roughly 70 steps of a 10,000-entity scene. Fine for the 2D
  scenes targeted now; very large worlds will need diff-based snapshots or
  per-chunk documents.
- New operations get undo for free if they run inside a transaction; one
  that forgets `markChanged()` produces no undo step, which is visible at
  once (nothing to undo, the scene does not become dirty).
- The editor model is covered by unit tests (`tests/test_editor.cpp`),
  including building and playing a game from an empty project without any
  UI.
- Asset operations and project settings are not part of scene undo;
  destructive asset operations ask for confirmation instead.
- Multi-user editing would need an operation log rather than snapshots; that
  is a later decision (Milestone 8).
