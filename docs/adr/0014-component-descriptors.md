# ADR 0014: Component descriptors as the single source of truth

- Status: Accepted
- Date: 2026-10-23

## Context
Until Milestone 2 every component had a hand-written JSON codec in the
project format, and documentation was written by hand. Studio needs an
inspector that edits every field with the right widget and limits, a way to
add and remove components safely, validation that matches what the loader
accepts, and a component reference that does not drift. Writing each of
these per component would multiply the cost of every new component and let
the loader, editor and docs disagree.

## Decision
- Every built-in component has a **descriptor** (`scene::ComponentInfo` in
  `engine/scene/include/px/scene/ComponentInfo.hpp`): name, category,
  description, the components it requires, and its fields. Each field
  (`FieldInfo`) has a name, label, description, kind (`Float`, `Int`, `Bool`,
  `String`, `Vec2`, `Rect`, `Color`, `Asset`, `EntityRef`, lists, and the
  special `TileGrid` and `ScriptList`), an accessor, flags (runtime-only,
  required, optional) and limits (number range, step, list counts, string
  lengths, asset type).
- The descriptors drive:
  - the **scene file format**: one generic reader and writer
    (`SceneFormat.cpp`) replace the per-component codecs;
  - **validation** (`validateEntity`): ranges, required assets, entity
    references, required components; used by the loader and by Studio
    before saving;
  - **Studio's inspector** and Add Component menu;
  - **reference documentation**: `px-headless --print-component-reference`
    generates [docs/components.md](../components.md), and a test fails when
    it is out of date;
  - reference **remapping** when entities are duplicated or pasted, and
    asset-use queries (`assetReferences`).
- Only requirements whose absence makes a scene invalid are declared
  (`PlatformerController` requires `Transform`, `BoxCollider` and
  `KinematicBody`); adding a requirement later would reject files that
  loaded before, so it needs a format version and migration.
- Descriptors are static tables in C++; there is no runtime registration and
  no reflection library.

## Alternatives considered
- **Keep hand-written codecs and write inspector code per component:**
  simplest per component, but three to four places to change for every
  field, and they drift (the reason this ADR exists).
- **Reflection library (e.g. refl-cpp, RTTR, meta from EnTT):** generic, but
  heavy template machinery or a runtime registry, and it would still need
  our own metadata (ranges, descriptions, asset types). Our needs fit in one
  table per component.
- **Code generation from a schema file (JSON/IDL):** single source too, but
  adds a build step and a second language for engine contributors.

## Consequences
- A new field is one line in the table: it is saved, loaded, validated,
  editable in Studio and documented. A new component needs its struct, one
  table entry and tests (see CONTRIBUTING).
- Field kinds are a closed set; a component with an unusual field needs a new
  kind (and a widget in Studio). `TileGrid` and `ScriptList` are such
  special kinds.
- The scripting API still has its own binding table
  ([ADR 0010](0010-script-execution-model.md)); exposing components to scripts
  generically through descriptors is possible later but is an API design
  decision, not automatic.
- Descriptors are the natural input for visual-scripting node generation and
  for network replication schemas (Milestones 4-5).
