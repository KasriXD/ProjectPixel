# Visual scripting design

**Status: design only. Nothing in this document is implemented yet.**
Planned for Milestone 5; this records the decisions that Milestone 2's
scripting API was shaped by, so the two systems stay one system.

## Goal

Beginners build behaviour by connecting nodes; experienced creators write
Luau. Both must be able to do the same things, mix in one project, and
move between the two without rewriting a game. Project principle: *do not
build two unrelated gameplay systems*.

## Core decision: one API, two front ends

Visual graphs are **compiled to Luau source** and run by the same VM,
sandbox, limits and API as handwritten scripts.

```
  node graph (.graph.json) ──compile──▶ Luau source ──Luau compiler──▶ bytecode ──▶ ScriptVm
  handwritten .luau ─────────────────────────────────▲
```

Why compile to Luau rather than interpret graphs:

- One execution engine: sandboxing, CPU and memory limits, determinism,
  realms and error reporting already exist and are tested.
- Graph performance equals script performance.
- "Convert graph to script" becomes a real export, and the generated code
  can be read when debugging.
- Visual nodes can call any Luau module, and Luau behaviours can be used in
  graphs, because both are behaviours with properties and lifecycle events.

Rejected alternative: a separate C++ graph interpreter. It would need its own
limits, sandbox, error reporting and determinism guarantees, and would drift
from the Luau API.

## Mapping the API to nodes

The node palette is generated from `scripting::apiReference()`
(`engine/scripting/src/Api.cpp`), the same table that registers the Luau
functions and generates [scripting-api.md](scripting-api.md):

| API member | Node kind |
|---|---|
| Lifecycle event (`Start`, `Tick`, `TriggerEnter`, ...) | **Event node**: an execution output plus data outputs for its arguments (`dt`, `other`) |
| Function or method with side effects (`Game.addScore`, `entity:destroy`) | **Action node**: execution in/out + typed data inputs |
| Member marked `pure` (`World.find`, `entity.position`, `Input.isDown`) | **Data node**: no execution pins, evaluated when its output is needed |
| Writable property (`entity.position`) | a "get" data node and a "set" action node |
| `Events.on` / `Events.emit` | "On custom event" event node / "Emit event" action node |
| Behaviour `properties` | graph variables exposed in the inspector |
| `authority` availability | nodes are hidden or flagged in client graphs |

To support this, every API entry already records: group, name, kind,
availability (realm), a typed Luau signature, a description, writability,
and whether it is pure. Still needed before the editor: machine-readable
parameter lists (name, type, default) instead of only the signature text.
This is an additive change to `ApiMember`.

## Types

Pin types follow the API's value types: `number`, `boolean`, `string`,
`vector`, `Entity`, plus `exec` for control flow. These are the same types
allowed for script properties and event values, so variables, properties
and events need no conversions. Connections are type-checked in the editor;
implicit conversions are limited to number → string for display.

## Graph file format (sketch)

A graph is an asset like a script and is attached with the same `Script`
component (its asset type tells the loader to compile it first):

```json
{
  "format": "px.graph", "formatVersion": 1,
  "properties": { "value": { "type": "number", "default": 1 } },
  "nodes": [
    { "id": "n1", "type": "event:TriggerEnter" },
    { "id": "n2", "type": "method:Entity.hasComponent", "inputs": { "name": "Player" } },
    { "id": "n3", "type": "flow:Branch" },
    { "id": "n4", "type": "function:Game.addScore" },
    { "id": "n5", "type": "method:Entity.destroy" }
  ],
  "links": [
    ["n1.other", "n2.self"], ["n1.exec", "n3.exec"], ["n2.result", "n3.condition"],
    ["n3.true", "n4.exec"], ["property:value", "n4.amount"], ["n4.exec", "n5.exec"], ["self.entity", "n5.self"]
  ]
}
```

That graph compiles to essentially `coin.luau` from the example project.
Node ids are stable so diffs and merges stay readable; editor-only data
(positions, comments, colours) goes in a separate `layout` section that the
compiler ignores.

## Flow-control and utility nodes

Provided by the graph compiler, not the engine API: Branch, Sequence,
For-each (over arrays such as `World.findWithTag`), Compare, Math, Vector
make/split, Variable get/set, Delay (implemented with a per-instance timer
in `Tick`; no coroutines in generated code for v1, which keeps debugging
and limits simple).

## Errors and debugging

- The graph compiler validates graphs (unconnected required inputs, type
  mismatches, cycles in data nodes) and reports errors by node id.
- Generated Luau carries a source map (generated line → node id), so a
  runtime error `graph.luau:14` is shown on the node that failed.
- Later: execution highlighting during play mode, using the same mapping.

## Prerequisites before building the editor

1. Structured parameter metadata in `ApiMember` (see above).
2. Studio foundation (Milestone 3): inspector, asset browser, play mode.
3. Multiple behaviours per entity, so graphs and scripts can be combined.
4. `require` for shared Luau modules, so graphs can call creator libraries.
