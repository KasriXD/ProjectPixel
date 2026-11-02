# Visual scripting

A **visual script** is a behaviour made of nodes connected by wires instead
of typed Luau. It is attached to entities exactly like a script, has
properties you can change per entity, and can sit next to Luau scripts on
the same entity. Under the hood each graph is compiled to Luau and run by
the same VM, sandbox, limits and API, so anything a graph does a script
could do too, and the other way round as far as the nodes reach
([ADR 0020](adr/0020-visual-scripting.md)).

The platformer example has one: `graphs/drift.graph.json` makes the clouds
drift slowly to the left and start again past the right edge of the level.

## Making one in Studio

1. *Assets > + New > Visual Script...*, give it a name. It opens in the
   **graph editor** with a starter graph: *Start* → *Print* "Hello from"
   and the name you gave it.
2. Attach it to an entity: drag it from Assets onto the entity (Scene view
   or Explorer), or *Add Script* in Properties, where graphs are listed with
   "(nodes)".
3. Press F5. The message appears in Output.

Double-click a visual script in Assets to open it again (the *Scripts*
filter shows scripts and visual scripts). The editor saves as you go; every
change is recompiled at once.

## The graph editor

| Input | Action |
|---|---|
| Right-click the canvas, *+ Add Node* | Add a node: type to search titles and ids, Enter adds the first match |
| Drag from a pin to a pin | Connect them. A data input takes one wire and an execution output continues one chain (use *Sequence* for more); a new wire replaces the old one |
| Drag from a pin to empty space | Opens the add menu showing only nodes that fit, and connects the new node |
| Alt+click a pin | Disconnect it |
| Click, Ctrl+click, drag on empty space | Select a node, add or remove one, select with a box (Ctrl adds) |
| Drag a node | Move it (and the rest of the selection) |
| Delete, Ctrl+D | Delete, duplicate the selection |
| Ctrl+Z, Ctrl+Y | Undo, redo (the editor's own history) |
| Mouse wheel; middle or right drag | Zoom; pan |

The panel on the right shows the selected node's description and lets you
type values into inputs that have no wire (numbers, text, true/false,
vectors). Below it are the graph's **properties**: *+ Property* adds one with
a name and a default; *Get Property* and *Set Property* nodes read and
change it. Properties appear in Properties for each entity the graph is on,
like a script's `properties` table.

The toolbar shows **Compiles** or the number of problems; problems are
listed under the canvas and the nodes they name are outlined in red.
*Show Luau* shows the code the graph runs as (read only).

## Nodes

There are three kinds:

- **Event** nodes (red header) start a chain when something happens:
  *Start*, *Tick* (with `dt`), *Trigger Enter*/*Trigger Exit* (with
  `other`), *Destroy*, *Player Joined*/*Player Left* (with `player`), *On
  Custom Event* (any script's `Events.emit` with that name) and *Every N
  Seconds*.
- **Action** nodes (blue; grey for flow control) do something. White
  triangles are the execution pins: a chain runs from an event through the
  actions wired one after another.
- **Data** nodes (green) have no execution pins; they compute a value
  whenever an input wired to them needs it (*Get Position*, *Add*, *Less
  Than*, *World Find*).

Pins are coloured by type: number, boolean, text, vector, entity, list of
entities, or any. A wire can only join pins whose types fit; an
unconnected input uses the value typed into the node, or a sensible empty
value (0, false, "", the zero vector). Entity inputs need a wire, except
`self`, which means *this entity* when it is left empty.

Most nodes are generated from the scripting API, so every function,
method and property in [scripting-api.md](scripting-api.md) is available, in
the same groups (*Game*, *World*, *Input*, *Events*, *Entity*, ...).
Writable properties give a *Get* and a *Set* node. Nodes that only the
authority may use (changing entities, spawning, scoring) say so in their
tooltip. The rest are built in:

| Node | What it does |
|---|---|
| Branch | Runs *true* or *false* depending on a condition |
| Sequence | Runs *first*, *second*, *third* in order |
| For Each Entity | Runs *body* once per entity of a list (`item`), then *done* |
| Repeat | Runs *body* `count` times (`index` from 1), then *done* |
| Delay | Runs *then* after a number of seconds; the rest of the chain does not wait |
| Every N Seconds | An event that fires on a timer |
| On Custom Event | An event for `Events.emit(name, value)` from any script or graph |
| This Entity | The entity the behaviour is on |
| Get Property / Set Property | The graph's own properties |
| Add, Subtract, Multiply, Divide, Random Number | Arithmetic |
| Equal, Not Equal, Less Than, Greater Than | Comparisons |
| And, Or, Not | Logic |
| Make Vector, Split Vector, Vector Length | Vectors |
| Join Text | Joins two values as text (handy with *Print*) |

### How a graph runs

- Each event node is a handler. Several event nodes of the same kind (two
  *Tick* nodes) run one after the other, in the order the nodes are in the
  file.
- Values from an action (a *World Spawn* result, a *For Each* item, an
  event's `other`) can only be used further along that action's own
  chain. Using them anywhere else is a problem the editor reports.
- A data node is computed **where it is used**: if one *Random Number*
  feeds two inputs, they get two different numbers. Store a value in a
  property with *Set Property* to use the same one twice, or to keep it
  until the next event.
- Chains cannot loop back on themselves (use *Repeat* or *For Each*), and
  a data node cannot depend on its own output.
- *Delay* and *Every N Seconds* count simulated time, so they are
  deterministic and pause with the game.

## Errors

The compiler checks a graph before it runs and names the node in every
message, for example `node 'Set Position' (n7): input 'value' (vector)
cannot take 'result' (number) of node 'Add' (n5)`. A graph with problems is
not loaded: the game reports it like a Luau compile error.

Runtime errors name the node that failed, not a line of generated code:

```
Script error in graphs/turret.graph.json (entity 'Turret') during Tick: graphs/turret.graph.json,
node 'World Spawn' (spawn): World.spawn: no prefab named 'Bullit' (did you mean 'Bullet'?)
```

## Files

A graph is a JSON asset (`px.graph`, version 1) of type `graph` in
`project.json`, usually under `graphs/`. Studio and
`px-headless --resave` write it with one line per node and link so diffs
stay readable:

```json
{
  "format": "px.graph",
  "formatVersion": 1,
  "properties": [
    {"name": "speed", "value": 6.0}
  ],
  "nodes": [
    {"id": "tick", "type": "event.Tick", "position": [0.0, 0.0]},
    {"id": "speed", "type": "property.get", "position": [180.0, 340.0], "property": "speed"},
    {"id": "say", "type": "call.print", "position": [200.0, 0.0], "inputs": {"value": "hi"}}
  ],
  "links": [
    {"from": "tick.then", "to": "say.in"}
  ]
}
```

- Node ids are any short identifier, unique in the graph; Studio uses
  `n1`, `n2`, ... They appear in error messages, so hand-written graphs can
  use meaningful ones.
- `type` is a node id from the catalog: `event.<Event>`, `call.<Group>.<name>`,
  `call.<global>`, `method.<name>`, `get.<property>`, `set.<property>`, or a
  built-in (`flow.branch`, `math.add`, ...). The editor's tooltips show them.
- `inputs` holds values typed into unconnected inputs (numbers, booleans,
  strings, `[x, y]` vectors).
- Limits: 2000 nodes and 8000 links per graph.

`px-headless --compile-graph graphs/drift.graph.json` prints the Luau a graph
compiles to, or its problems.

## Limitations

- No functions or sub-graphs yet: repeated logic is copied, and a chain
  that joins another chain's nodes again is compiled twice.
- No local variables beyond properties, no tables, no `require` of Luau
  modules from a graph.
- No comments, groups or reroute points on the canvas.
- No conversion from Luau to a graph (*Show Luau* goes the other way).
- No execution highlighting while playing; runtime errors name the node.
- Like scripts, graphs run on the authority only (single player and the
  server); client-side behaviours are planned.
