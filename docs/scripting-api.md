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
| `function Behaviour:PlayerJoined(player: Entity)` | event | all | A player joined; `player` is their character. Every behaviour receives it, after the character's own Start. Playing alone, it runs once for the player at the start. |
| `function Behaviour:PlayerLeft(player: Entity)` | event | all | A player left the game. Their character is still readable here and is removed right after. |

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
| `Game.getObjective(): string` | function, pure | all | The objective line currently shown on the HUD (empty if none). |
| `Game.setObjective(text: string)` | function | authority | Sets the objective line shown on the HUD (at most 200 characters). |
| `Game.completeLevel(message: string?)` | function | authority | Ends the level successfully and shows the message. |
| `Game.time(): number` | function, pure | all | Simulated seconds since the session started (deterministic; not the wall clock). |
| `Game.tickCount(): number` | function, pure | all | Number of ticks simulated so far. |

## World

Finding entities in the running scene.

| Member | Kind | Available in | Description |
|---|---|---|---|
| `World.find(name: string): Entity?` | function, pure | all | The first entity with this name (in hierarchy order), or nil. |
| `World.findById(id: string): Entity?` | function, pure | all | The entity with this id, or nil. |
| `World.findWithTag(tag: string): {Entity}` | function, pure | all | All entities with the tag, in hierarchy order. |

## Input

Input actions defined in the project's `input.actions`.

| Member | Kind | Available in | Description |
|---|---|---|---|
| `Input.isDown(action: string, player: Entity?): boolean` | function, pure | all | True while the action is held this tick: by `player` (a character) if given, otherwise by any player. Unknown action names are errors. |
| `Input.wasPressed(action: string, player: Entity?): boolean` | function, pure | all | True on the tick the action went down (for `player`, or for any player). |

## Players

The people playing. Each has a character: a copy of the scene's Player entity in multiplayer, the scene's Player entity when playing alone.

| Member | Kind | Available in | Description |
|---|---|---|---|
| `Players.getAll(): {Entity}` | function, pure | all | Every player character (entities with a Player component), in the order they joined. |
| `Players.count(): number` | function, pure | all | How many player characters there are. |

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
| `entity.position: vector` | property (read/write), pure | all | World position (Transform). Y points down. Assigning teleports the entity; its children move with it. |
| `entity.localPosition: vector` | property (read/write), pure | all | Position relative to the parent (the Transform's own value). Same as position for entities without a parent. |
| `entity.rotation: number` | property (read/write), pure | all | World rotation in degrees, clockwise (Transform). |
| `entity.localRotation: number` | property (read/write), pure | all | Rotation relative to the parent, in degrees. |
| `entity.scale: vector` | property (read/write), pure | all | Scale relative to the parent (Transform). Negative values mirror. |
| `entity.parent: Entity?` | property (read-only), pure | all | The parent entity, or nil for top-level entities. Change it with setParent. |
| `entity.velocity: vector` | property (read/write), pure | all | Velocity in units per second (KinematicBody). |
| `entity.onGround: boolean` | property (read-only), pure | all | True if the body is standing on something (KinematicBody). |
| `entity.visible: boolean` | property (read/write), pure | all | Whether the Sprite is drawn. |
| `entity.flipX: boolean` | property (read/write), pure | all | Mirrors the Sprite horizontally. |
| `entity.spawnPoint: vector` | property (read/write), pure | all | Where a Player respawns (its starting position unless changed). Set it in a checkpoint to make the player come back there. |
| `entity.playerName: string` | property (read-only), pure | all | The name of the player controlling this character (playing alone: the character's name). |
| `entity.playerId: number` | property (read-only), pure | all | The id of the player controlling this character in multiplayer (1, 2, ...; 0 when playing alone). |
| `entity:isValid(): boolean` | method, pure | all | False once the entity has been destroyed. Other members raise an error on destroyed entities. |
| `entity:destroy()` | method | authority | Removes the entity and its children at the end of the current phase (after the running handlers). |
| `entity:respawn()` | method | authority | Sends a Player back to its spawn point and stops it, as when it falls off the level (counts as a respawn). Use it for hazards and enemies. Errors for entities without a Player component. |
| `entity:hasTag(tag: string): boolean` | method, pure | all | True if the entity's Tags component contains the tag. |
| `entity:hasComponent(name: string): boolean` | method, pure | all | True if the entity has the named component, e.g. "Player" (see docs/components.md). |
| `entity:getChildren(): {Entity}` | method, pure | all | The entity's direct children, in hierarchy order. |
| `entity:setParent(parent: Entity?)` | method | authority | Moves the entity (with its children) under another entity, or to the top level with nil. It stays where it is in the world. Errors if the parent is the entity itself or one of its descendants. |
