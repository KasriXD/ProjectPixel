# ADR 0006: Project format: versioned JSON with GUID references

- Status: Accepted
- Date: 2026-10-22

## Context
Games are content, separate from engine code. Files must survive engine
upgrades, be diffable in version control, be safe to load from untrusted
sources, and avoid fragile path references.

## Decision
- A project is a folder with `project.json` (manifest), scene files and
  assets. Format reference: `docs/project-format.md`.
- **JSON**, pretty-printed with numeric arrays inline, deterministic output.
- Each file has a `format` tag and an integer `formatVersion`. Older versions
  are upgraded by **migration functions on the JSON** before parsing; newer
  versions are refused with an "update the engine" message.
- All references between files and objects are **GUIDs**. Paths appear only
  in the manifest's asset list, are normalized and confined to the project.
- **Strict validation**: unknown fields, wrong types, bad ranges and dangling
  references are load errors naming file and JSON path, with spelling
  suggestions. Hard limits on sizes.
- Writers emit all authored fields, so changing a default never changes the
  meaning of saved files.
- Tilemaps store rows as strings (`"0 1 . 2"`) for readable diffs.

## Alternatives considered
- **Binary format:** smaller and faster, but not diffable or hand-editable.
  Planned as an export format for published builds, generated from JSON.
- **YAML/TOML:** more pleasant to hand-write, but ambiguous edge cases (YAML)
  or weak for nested data (TOML).
- **Path-based references:** break on rename/move.
- **Sidecar `.meta` files per asset (Unity-style):** fewer merge conflicts
  for teams; the central asset list can migrate to sidecars later via a
  format version bump.

## Consequences
- Hand-editing is practical until Studio exists, with good error messages.
- GUIDs in files are less readable than names; Studio will hide them.
- Collaboration-friendly asset metadata (sidecars) is a likely future
  migration.
