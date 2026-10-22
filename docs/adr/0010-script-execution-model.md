# ADR 0010: Script execution model

- Status: Accepted
- Date: 2026-10-23

## Context
Scripts must be easy for beginners, safe to run untrusted, deterministic
for multiplayer and replays, cheap per tick, and representable as visual
nodes later. The model chosen now shapes all game content written from here on.

## Decision
- **Behaviour tables.** A script returns a table with lifecycle functions and
  declared `properties`. Each entity gets an instance table (`self`) that
  inherits from it. Lifecycle events: `Start`, `Tick(dt)`, `TriggerEnter`,
  `TriggerExit`, `Destroy`. Named handler functions map directly to visual
  "event nodes" and are resolved once (no per-call lookups).
- **Declared, typed properties.** Scenes may only override declared
  properties with matching types; anything else is a load error.
- **One VM per session**, each script file in its own sandboxed environment
  (`luaL_sandboxthread`), all engine tables read-only.
- **Fixed, documented order**: Start, then Tick, in entity creation order;
  then physics; then trigger events in a stable order. Destruction and
  custom events are deferred to the end of each phase (no re-entrancy).
- **Failure policy**: content errors (syntax, bad properties, module errors)
  fail loading; runtime errors disable only the failing instance and are
  reported once with file, line, entity, event and traceback.
- **Limits**: memory cap via allocator (64 MiB per session); CPU budget per
  callback as a deterministic step count (5 M safepoints) with a 0.5 s
  wall-clock backstop; caps on events, log output, source size.
- **Determinism**: no clock or OS access; simulation time from the tick
  counter; `math.random` seeded per session.
- **Custom events** carry only serializable values (nil, number, boolean,
  string, vector, Entity).
- **One API table** (`kBindings` in `Api.cpp`) registers functions, decides
  realm availability and generates the API reference (kept current by a test).

## Alternatives considered
- **Connection-based events everywhere** (`entity.Touched:Connect(fn)`, as in
  Roblox): flexible, but harder to map to visual event nodes and easier to
  leak connections. Offered only for custom events (`Events.on`), with
  automatic cleanup when the owner is destroyed or fails.
- **Retrying failed instances every tick:** floods logs and can make
  broken state worse; fail-stop per instance is easier to understand.
- **Time-only CPU limits:** not deterministic; a server and a replay could
  disagree on whether a script ran.
- **Coroutines/yielding handlers (wait()):** convenient, but complicate
  limits, determinism and visual scripting. Not supported in v1; timers can
  be written in `Tick`.

## Consequences
- Behaviour can be changed without recompiling the engine; `px-player`
  reloads scripts with F5.
- Per-instance fail-stop means a broken script makes its objects inert, not
  the game unplayable; tools must make errors visible (on-screen panel today,
  Studio console later).
- The model needs extending for: multiple behaviours per entity, `require`,
  spawning entities, per-tick total CPU budgets for servers.
