# Scripting API reference (version 1)

<!-- Generated from engine/scripting/src/Api.cpp by `px-headless --print-api-reference`.
     Do not edit by hand: the test "API reference document is up to date" fails if this
     file differs from the generated text. -->

Guide and examples: [scripting.md](scripting.md). *Available in*: **all** means authority and client scripts; **authority** means only where game state is decided (single-player session, server). *Pure* members have no side effects (data nodes in visual scripting).

## Lifecycle

Functions a behaviour table may define. The engine calls them; see the lifecycle rules in scripting.md.

| Member | Kind | Available in | Description |
|---|---|---|---|
| `function Behaviour:Start()` | event | all | Runs once, before the first Tick, after every entity of the scene exists. Set up state here. |
| `function Behaviour:Tick(dt: number)` | event | all | Runs every simulation tick (60 per second by default). dt is the fixed tick length in seconds. Only behaviours that define Tick cost anything per tick. |
| `function Behaviour:TriggerEnter(other: Entity)` | event | all | A moving body (KinematicBody) started overlapping a trigger collider. Both entities receive it. |
| `function Behaviour:TriggerExit(other: Entity)` | event | all | The overlap ended, or the other entity was destroyed. |
| `function Behaviour:Destroy()` | event | all | The entity is about to be removed. It is still readable here. Not called when the whole session ends. |

## Globals

Global functions.

| Member | Kind | Available in | Description |
|---|---|---|---|
| `Vec2(x: number, y: number): vector` | global, pure | all | Creates a 2D vector (a Luau vector with z = 0). Supports + - * / and the vector library. |
| `print(...: any)` | global | all | Writes a line to the game log, prefixed with the script name. Limited to 100 lines per tick. |
| `warn(...: any)` | global | all | Like print, as a warning. |

## Game

Game state. Changes are authoritative and, in multiplayer, will be replicated to clients.

| Member | Kind | Available in | Description |
|---|---|---|---|
| `Game.addScore(amount: number)` | function | authority | Adds a whole number (may be negative) to the score. |
| `Game.getScore(): number` | function, pure | all | The current score. |
| `Game.setObjective(text: string)` | function | authority | Sets the objective line shown on the HUD (at most 200 characters). |
| `Game.completeLevel(message: string?)` | function | authority | Ends the level successfully and shows the message. |
| `Game.time(): number` | function, pure | all | Simulated seconds since the session started (deterministic; not the wall clock). |
| `Game.tickCount(): number` | function, pure | all | Number of ticks simulated so far. |

## World

Finding entities in the running scene.

| Member | Kind | Available in | Description |
|---|---|---|---|
| `World.find(name: string): Entity?` | function, pure | all | The first entity with this name, or nil. |
| `World.findById(id: string): Entity?` | function, pure | all | The entity with this id, or nil. |
| `World.findWithTag(tag: string): {Entity}` | function, pure | all | All entities with the tag, in creation order. |

## Input

Input actions defined in the project's `input.actions`.

| Member | Kind | Available in | Description |
|---|---|---|---|
| `Input.isDown(action: string): boolean` | function, pure | all | True while the action is held this tick. Unknown action names are errors. |
| `Input.wasPressed(action: string): boolean` | function, pure | all | True on the tick the action went down. |

## Events

Custom events between scripts. Values must be serializable.

| Member | Kind | Available in | Description |
|---|---|---|---|
| `Events.emit(name: string, value: any?)` | function | all | Queues an event. Listeners run after the current phase, in registration order. value must be nil, number, boolean, string, vector or Entity. |
| `Events.on(name: string, handler: (value: any?) -> ()): number` | function | all | Registers a listener and returns its id. Listeners registered by a behaviour are removed when its entity is destroyed or its script fails. |
| `Events.off(id: number): boolean` | function | all | Removes a listener. Returns false if it did not exist. |

## Entity

An `Entity` is a handle to a scene entity. Properties need the named component; accessing a destroyed entity is an error except `isValid()`. Writable properties can only be assigned by authority scripts.

| Member | Kind | Available in | Description |
|---|---|---|---|
| `entity.id: string` | property (read-only), pure | all | Stable unique id (from the scene file). |
| `entity.name: string` | property (read-only), pure | all | Display name. |
| `entity.position: vector` | property (read/write), pure | all | World position (Transform). Y points down. Assigning teleports the entity. |
| `entity.rotation: number` | property (read/write), pure | all | Rotation in degrees, clockwise (Transform). |
| `entity.scale: vector` | property (read/write), pure | all | Scale (Transform). |
| `entity.velocity: vector` | property (read/write), pure | all | Velocity in units per second (KinematicBody). |
| `entity.onGround: boolean` | property (read-only), pure | all | True if the body is standing on something (KinematicBody). |
| `entity.visible: boolean` | property (read/write), pure | all | Whether the Sprite is drawn. |
| `entity.flipX: boolean` | property (read/write), pure | all | Mirrors the Sprite horizontally. |
| `entity:isValid(): boolean` | method, pure | all | False once the entity has been destroyed. Other members raise an error on destroyed entities. |
| `entity:destroy()` | method | authority | Removes the entity at the end of the current phase (after the running handlers). |
| `entity:hasTag(tag: string): boolean` | method, pure | all | True if the entity's Tags component contains the tag. |
| `entity:hasComponent(name: string): boolean` | method, pure | all | True if the entity has the named component, e.g. "Player". |
