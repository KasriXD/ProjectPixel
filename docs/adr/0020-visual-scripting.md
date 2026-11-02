# ADR 0020: Visual scripting compiles graphs to Luau

- Status: Accepted
- Date: 2026-11-02

## Context

Milestone 5 adds visual scripting for creators who do not write code. The
design written in Milestone 2 ([visual-scripting.md](../visual-scripting.md)
before this milestone) fixed the main constraint: *do not build two unrelated
gameplay systems*. Graphs and scripts must reach the same API, mix on one
entity, keep the same sandbox, limits, determinism and authority rules, and
report errors that a creator can act on.

Options: (a) interpret graphs in C++ with their own runtime; (b) compile
graphs to Luau source and run it in the existing VM; (c) compile graphs to
Luau bytecode directly.

## Decision

1. **Graphs compile to Luau source** (`scripting::compileGraph`), which then
   goes through the normal Luau compiler and `ScriptVm`. A graph is a
   behaviour like any script: same `Scripts` component, properties,
   lifecycle events, realms and limits. Bytecode generation (c) was rejected
   because readable source is the debugging and teaching story ("Show Luau",
   `px-headless --compile-graph`) and the Luau compiler's checks come free.
2. **The node catalog is generated from `apiReference()`**: every API entry
   now has a machine-readable signature (`parseApiSignature`: typed
   parameters, optional ones, variadics, results), and functions, methods,
   readable and writable properties and events become nodes automatically.
   A new API function is a new node without editor work. Built-in nodes cover
   flow control, math, comparison, logic, vectors, text, properties, timers
   and custom events. `Events.on` (it takes a function) is the one API member
   replaced by a node (*On Custom Event*).
3. **Execution model**: event nodes become handlers (several of one kind run
   in file order); action chains become statements; data nodes are inlined as
   expressions at each use. Values produced by actions (results, loop items,
   event arguments) are locals visible only further along their chain, which
   the compiler checks. Timers (*Delay*, *Every N Seconds*) are a small
   per-instance timer list pumped from `Tick` with simulated `dt`, not
   coroutines, so they stay deterministic and need no new VM features.
4. **Errors name nodes**: compile problems carry the node id and say
   `node 'Title' (id)`. The compiler records, per generated line, the node it
   came from (`CompiledGraph::lineNodes`); `CompiledScript::locate` rewrites a
   runtime error's `file:line:` to that node.
5. **File format** `px.graph` v1: properties, nodes (id, type, position,
   property, typed-in input values) and links (`node.pin` to `node.pin`).
   Written one node or link per line so diffs and merges stay readable.
   Editor layout (positions) lives in the same file; the compiler ignores it.

## Consequences

- Graph performance, sandboxing, CPU and memory limits, determinism and
  authority checks are exactly those of scripts, with no second
  implementation to test or secure.
- Inlined data nodes mean a value used twice is computed twice (two
  different random numbers); creators store values in properties when that
  matters. Caching pure expressions in locals is a later optimisation.
- Chains that join again are emitted twice (the compiler stops past 100 000
  lines). Functions/sub-graphs are the planned answer.
- Changing the API can break saved graphs (a renamed parameter becomes an
  unknown pin). The compiler reports such graphs by node rather than
  failing silently; API changes need the same care as for scripts.
- Not yet: sub-graphs, local variables, comments and groups, Luau-to-graph
  conversion, execution highlighting while playing, client-realm graphs.
