# Scripting guide (API version 1)

Games customise behaviour with **Luau** scripts attached to entities. Luau
is the language; the functions scripts call (`Game`, `World`, `Entity`, ...)
are Project Pixel's own gameplay API. Full API list:
[scripting-api.md](scripting-api.md) (generated from the engine source).

Status: implemented in Milestone 2. Verified on Linux (GCC, Clang,
AddressSanitizer), Windows (MSVC in CI, MinGW under Wine) and macOS (CI).

## Quick start

1. Write a behaviour, e.g. `scripts/spinner.luau`:

   ```lua
   local Spinner = {
       properties = {
           speed = 90, -- degrees per second; scenes can override this
       },
   }

   function Spinner:Tick(dt)
       self.entity.rotation += self.speed * dt
   end

   return Spinner
   ```

2. List it in `project.json`:

   ```json
   { "id": "<new guid>", "type": "script", "name": "Spinner", "path": "scripts/spinner.luau" }
   ```

3. Attach it to an entity: in Studio, select the entity, choose **Add
   Script** in the Properties panel and pick it (Properties then lists its
   values). In a scene file the same looks like:

   ```json
   "Scripts": [ { "script": "<the script's asset id>", "properties": { "speed": 180 } } ]
   ```

4. Run `px-player`. While it runs, edit the script and press **F5**: the
   project, scenes and scripts are reloaded from disk without recompiling
   or restarting the program. If the new version has an error, the current
   game keeps running and the error is shown on screen.

The example project has three scripts worth reading:
[coin.luau](../examples/platformer/scripts/coin.luau) (trigger + score),
[moving_platform.luau](../examples/platformer/scripts/moving_platform.luau) (Tick + movement),
[level_rules.luau](../examples/platformer/scripts/level_rules.luau) (events + win condition).

The starter library behind Studio's Toolbox has more, each short and
commented, in [library/scripts](../library/scripts):
[hazard.luau](../library/scripts/hazard.luau) (sends the player back with `entity:respawn()`),
[checkpoint.luau](../library/scripts/checkpoint.luau) (moves `entity.spawnPoint`),
[key.luau](../library/scripts/key.luau) and [lock_block.luau](../library/scripts/lock_block.luau)
(one script opening others through an event),
[patrol.luau](../library/scripts/patrol.luau), [spring.luau](../library/scripts/spring.luau),
[goal.luau](../library/scripts/goal.luau) and [sign.luau](../library/scripts/sign.luau).

## Behaviours and instances

A script file is a **module**: its top-level code runs once when the game
loads, and it must `return` a **behaviour table**. The table may contain:

- lifecycle functions (`Start`, `Tick`, `TriggerEnter`, `TriggerExit`, `Destroy`),
- a `properties` table of defaults,
- any helper functions (`function Coin:award() ... end`).

Every script attached to an entity (its `Scripts` component) gets its own
**instance**: a table whose fields are `entity` (the owning Entity) plus
every property, and which inherits the behaviour's functions. Inside
handlers `self` is the instance, so per-entity state lives on `self`
(`self.collected += 1`). An entity can have several scripts (up to 16), for
example a `Coin` and a `Bob` behaviour; each is a separate instance with its
own properties, and they run in the order they are attached.

**Properties** are how scenes configure a behaviour without code. Defaults
are declared in `properties`; the values set in the scene (Studio's
Properties panel, or `properties` in the file) override them.
Allowed types are number, boolean, string and vector (`Vec2(x, y)` in Luau,
`[x, y]` in JSON). A scene that sets an undeclared property or the wrong type
fails to load with a message like:

```
Entity 'Moving Platform': scripts/moving_platform.luau has no property 'sped' (did you mean 'speed'?)
```

This strictness is deliberate: it is also what a future property inspector
(Studio) and visual-scripting nodes will rely on.

## Lifecycle

| Event | When | Arguments |
|---|---|---|
| `Start()` | once, at the beginning of the first tick after the instance exists; every scene entity already exists | |
| `Tick(dt)` | every tick, after all `Start`s of that tick | fixed tick length in seconds |
| `TriggerEnter(other)` | a moving body (`KinematicBody`) begins to overlap a trigger collider; **both** entities receive it | the other Entity |
| `TriggerExit(other)` | the overlap ends, or the other entity is destroyed | the other Entity |
| `Destroy()` | just before the entity is removed (it is still readable) | |

Behaviours only pay for what they define: an instance without `Tick` costs
nothing per tick.

### Execution order

Within one tick (`GameSession::tick`):

1. `Start` for every instance that has not started, in **hierarchy order**
   (the order of Studio's Explorer panel: parents before children), and for
   an entity's several scripts in attachment order.
2. `Tick` for every started instance, in the same order.
3. Moving solids carry the bodies standing on them.
4. Platformer controllers, then kinematic movement and collision.
5. Trigger overlaps are compared with the previous tick: exits first, then
   enters, each in a stable order; each pair is delivered to the trigger's
   script, then to the other entity's script.
6. Kill plane, sprite animation, cameras.

After phases 1, 2 and 5 the engine **flushes**: entities destroyed during
the phase are removed together with their children (partners get
`TriggerExit`; the entity and then each descendant get `Destroy`), then
queued custom events are delivered. Flushing repeats until nothing is left
(bounded by the event limit).

Consequences worth knowing:

- `entity:destroy()` is deferred: the entity stays valid until the phase ends.
- Events sent with `Events.emit` are delivered after the phase, never
  re-entrantly inside `emit`.
- Order depends only on the scene, so a game behaves identically every run.

### Errors and failure policy

- **Load-time errors stop the game from starting** (like a broken scene
  file): syntax errors, a module that errors or does not return a table,
  non-function lifecycle fields, unknown or mistyped properties, a script
  attachment that refers to a missing asset. Messages name the file and line.
- **Runtime errors disable only the failing instance.** The error is logged
  with script, entity, event, `file:line` and a traceback, counted in
  `GameState::scriptErrors`, raised as a `ScriptError` game event and shown
  on screen by `px-player`. The instance receives no further callbacks and
  its event listeners are removed; everything else keeps running.
  One broken coin does not stop the game, and a broken `Tick` does not
  flood the log 60 times a second.
- Misspelled lifecycle functions (`function Coin:Tik()`) produce a warning
  at load, because they would otherwise silently never run.
- When a session ends (restart, reload, quit), instances are released
  without calling `Destroy`.

## Sandbox

Scripts are untrusted: they come from creators and will be downloaded by
players. The sandbox is enforced by the engine, not by convention.

**Not available to scripts** (tested in `tests/test_scripting.cpp`):
`io`, `os`, `debug`, `require`, `package`, `load`/`loadstring`, `dofile`,
`loadfile`, `getfenv`, `setfenv`, `collectgarbage`, `gcinfo`, `newproxy`.
There is no file, network, process, clock or environment access of any
kind. Native code generation (JIT) is not compiled in.

**Available:** the Luau base library (minus the above), `string`, `table`,
`math`, `utf8`, `bit32`, `buffer`, `vector`, `coroutine`, and the engine API.

**Isolation:**

- Every built-in and engine table is read-only (`math.floor = nil` fails),
  and so are the shared string and vector metatables.
- Each script file has its own global environment: a global set by one
  script is invisible to others. (`_G` is the shared, read-only table.)
- `Entity` values are opaque handles with a locked metatable; scripts
  cannot add fields to them or replace their behaviour.
- Bytecode is only produced by the engine's compiler from source. The VM
  never loads bytecode from elsewhere (Luau does not verify bytecode).

### Resource limits

All limits live in `scripting::ScriptLimits` and are per session (one VM):

| Limit | Default | When exceeded |
|---|---|---|
| Memory for all scripts | 64 MiB | the allocation fails, the running callback errors with "out of memory", garbage is collected, the instance is disabled |
| Steps per callback (loop iterations + calls) | 5,000,000 | the callback errors with "exceeded its CPU budget"; catching that error with `pcall` does not help, every later step fails too |
| Wall-clock time per callback | 0.5 s | as above ("ran longer than 500 ms"); backstop for slow non-looping code |
| Event deliveries per tick | 10,000 | remaining events are dropped and an error is reported |
| `print`/`warn` lines per tick | 100 (1,000 chars each) | further lines are dropped with one notice |
| Script source size | 1 MiB | the project fails to load |
| Call depth | 20,000 nested calls (Luau) | "stack overflow" error |

The step limit is **deterministic**: the same script fails at the same
point on every machine, which matters for servers and replays. The
wall-clock limit is not deterministic and only exists as a safety net.

### What the sandbox does not (yet) protect against

Be clear about the remaining risks:

- **Total CPU per tick.** Limits are per callback, so 10,000 instances each
  using most of their budget can still stall a tick. A per-tick budget
  belongs with server hosting (Milestone 4/6).
- **Luau VM bugs.** The VM is a dependency; a bug in it could escape the
  sandbox. Mitigations: keep Luau updated (`tools/update_luau.py`), run game
  servers in isolated containers (Milestone 6), fuzz the script loader.
- **Engine API bugs.** Each binding validates its arguments, but new API
  must be reviewed with the same care (see CONTRIBUTING.md).
- **Host memory outside the VM.** Entity handles and queued events are
  bounded by the scene size and the event limits, not by the 64 MiB cap.

## Determinism

Simulation results must not depend on the machine (replays, tests,
server/client agreement). Scripts get only deterministic inputs:
`Game.time()` is simulated time, not the clock; there is no `os.time` or
`os.clock`; `math.random` is seeded from the session seed. The scripted
example level is replayed twice in a test and must end bit-identical; it
also ends at the same position on Linux and Windows builds.

## Multiplayer: realms and authority

Scripts run in a **realm**:

- **Authority**: where game state is decided. Today that is the
  single-player session; in multiplayer it will be the dedicated server.
- **Client**: a player's machine (planned, Milestone 4), for presentation
  and prediction only.

Every API member is marked *all* or *authority* in
[scripting-api.md](scripting-api.md). Authority-only functions are **not
registered at all** in client VMs, and client scripts cannot assign entity
properties. This is enforced and tested now (a client-realm VM is created
in the tests), so content written today does not accidentally rely on
client-side authority.

| Today | Realm | Why |
|---|---|---|
| `Game.addScore`, `Game.setObjective`, `Game.completeLevel` | authority only | score, objectives and progress are trusted state |
| `entity:destroy()`, `entity:respawn()`, assigning `entity.position` etc. | authority only | entity state is replicated from the server |
| queries (`World.find`, `entity.position`, `Game.getScore`, `Game.getObjective`, `Input.isDown`) | all | reading is safe; on clients they read replicated state |
| `Events.emit/on/off` | all | each VM has its own event bus; events never cross the network implicitly |

Planned for Milestone 4 (not implemented): a per-script setting
for where a behaviour runs (`server`, `client`, `both`), explicit network
messages (client-to-server *requests* that the server validates, like
Roblox RemoteEvents), per-player input on the server (`Input` becomes
per-player), and client-local presentation entities. Event values are
already restricted to serializable types so that events can later cross
the network without changing the API.

## API versioning

- The engine provides scripting API version 1
  (`project::kScriptApiVersion`). Projects declare the version they target
  in `settings.scripting.apiVersion`; a project that needs a newer version
  is refused with a message asking to update the engine.
- Adding functions, properties or events is backwards compatible and keeps
  the version.
- Renaming, removing or changing the meaning of a member is a breaking
  change: it requires a new API version, and the old version must keep
  working for existing projects (or be migrated) until it is retired.

## Performance notes

- Lifecycle handlers are resolved once per behaviour; calls do no string
  lookups.
- An Entity handle is created once per entity and reused, so reading
  `self.entity` allocates nothing; positions are native Luau vectors
  (no allocation).
- Measured (test "Performance: scripted Tick cost stays small"): 1,000
  instances with a `Tick` that reads and writes rotation and position, plus
  5,000 instances without `Tick`: about 0.7 ms per tick in an optimised
  build (6 ms in a debug build). Script memory: about 1.2 MiB.
- Trigger detection checks every trigger against every moving body each
  tick. That is fine at the current scale; a spatial broadphase is needed
  before large worlds.

## Visual scripting

Not implemented yet. The API was designed for it: see
[visual-scripting.md](visual-scripting.md).

## Hierarchy

Entities can have a parent (drag them onto each other in Studio's Explorer,
or `parent` in the scene file). In scripts:

- `entity.position` and `entity.rotation` are **world** values; assigning
  them moves the entity in the world (its children follow).
- `entity.localPosition` and `entity.localRotation` are relative to the
  parent (the `Transform` values); `entity.scale` is always relative.
- `entity.parent` is the parent or `nil`; `entity:getChildren()` lists the
  direct children; `entity:setParent(other)` (authority only) re-parents the
  entity and keeps it where it is in the world.
- `entity:destroy()` also destroys the children.

For entities without a parent nothing changed: world and local values are
the same, so version 1 scripts behave exactly as before.

## Limitations of version 1
- No `require`: scripts cannot share code modules yet. Planned as
  `require` of script assets by id, resolved by the engine.
- Scripts cannot create entities yet (needs prefabs, Milestone 3/5).
- Scripts can read and write only the members listed in the API reference;
  component access is not generic yet.
- No script debugger or breakpoints; errors carry file, line and traceback.
- Moving solids carry bodies standing on them but do not push bodies
  sideways.
