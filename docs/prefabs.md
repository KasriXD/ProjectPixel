# Prefabs guide

A **prefab** is a reusable object: an entity with its children, components
and scripts, saved as an asset. Place copies of it in any scene, change the
prefab once and every copy follows, and let scripts create copies while the
game runs (bullets, effects, enemies). Roblox creators know the idea from
packages and from cloning objects out of ReplicatedStorage.

Status: **Milestone 5**. File format: [project-format.md](project-format.md#prefab-files).

## In Studio

**Make one.** Right-click an entity in the Explorer and choose *Save As
Prefab...*, then give it a name. The entity and everything under it are
saved as `prefabs/<name>.prefab.json`, and the entity becomes the first copy.
Copies are shown in purple in the Explorer.

**Place copies.** Drag the prefab from *Assets* into the Scene view (or onto
an entity in the Explorer to put the copy under it), double-click it, or
right-click it and choose *Place In Scene*.

**Change every copy.** Edit any copy as usual, then press *Apply To Prefab*
in Properties (or right-click it in the Explorer). The prefab is saved and
every other copy is updated: in the open scene, in the project's other
scenes and inside other prefabs. Each copy keeps **its own changes**:

| A copy keeps | Example |
|---|---|
| values changed on it | a bigger collider, another tint, a script property like `value = 5` |
| its name and position | always, since every copy stands somewhere else |
| components added to or removed from it | a copy that also has a `KinematicBody` |
| entities added to it, entities deleted from it | a coin with an extra sparkle child |

Everything else comes from the prefab, including new children and new
components. A change counts as the copy's own when its value differed from
the prefab's before the update.

**Throw a copy's changes away** with *Revert* (Properties) or *Revert To
Prefab* (Explorer): only its position, name and place in the hierarchy stay.

**Unlink a copy** by removing its `PrefabInstance` component: it becomes
ordinary entities that no longer follow the prefab.

Notes:

- Applying writes the prefab file and the other scene and prefab files at
  once (Output lists them). *Undo* reverts the open scene only.
- Prefabs may contain copies of other prefabs. A prefab cannot contain a
  copy of itself.
- *Assets > right-click > Find Uses* lists the copies; a prefab with copies
  cannot be deleted. Scripts that spawn it by name are not found this way:
  renaming a prefab breaks `World.spawn` calls that use the old name.
- Copy and paste or *Duplicate* a copy and the result is a copy too.

## Spawning from scripts

```lua
-- A turret that fires the "Bullet" prefab every second.
local Turret = {
    properties = { interval = 1, speed = 120 },
}

function Turret:Start()
    self.timer = 0
end

function Turret:Tick(dt)
    self.timer += dt
    if self.timer >= self.interval then
        self.timer = 0
        local bullet = World.spawn("Bullet", self.entity.position + Vec2(8, 0))
        bullet.velocity = Vec2(self.speed, 0) -- the Bullet prefab has a KinematicBody
    end
end

return Turret
```

`World.spawn(prefab, position?, parent?)` creates a copy of the prefab named
`prefab` (its asset name) and returns its root entity:

- `position` is the world position of the root; by default the parent's
  position, or `[0, 0]`.
- With `parent`, the copy becomes that entity's child and moves with it.
- The entities exist at once: set their properties, find them with
  `World.find`, destroy them. Their own scripts start at the beginning of
  the **next tick** (`Start`, then `Tick`), like the scripts of players who
  join.
- Spawned tilemaps are solid from the next physics step; destroying a
  tilemap removes its collision.
- Ids are deterministic: the same session spawns the same ids in the same
  order, so replays and tests are repeatable.
- Limits: 1000 spawned entities per tick (a prefab's children count) and
  100 000 entities in a scene. Exceeding them is a script error at the
  `World.spawn` call.
- Only authority scripts (single player, the server) can spawn.

In multiplayer, spawning happens on the server and the new entities reach
every player in the next snapshots, like any other change.

The examples use both halves: every coin of the platformer (and every gem
of the top-down garden) is a copy of a *Coin* (*Gem*) prefab, and the
`coin.luau` behaviour spawns its `effect` prefab, a *Sparkle* that rises
and fades ([sparkle.luau](../examples/platformer/scripts/sparkle.luau)),
where a coin was collected.

## How copies are tracked

A copy's root has a `PrefabInstance` component; nothing else is stored.
The ids of a copy's entities are the prefab's ids XOR a key derived from
the copy's root id, so Studio can tell which prefab entity each entity came
from (and which ones were added or deleted) by arithmetic alone. Copying a
copy re-keys it. Updating compares each copy with the prefab's previous
content, field by field, so no list of overrides has to be kept in sync.
Details: [ADR 0019](adr/0019-prefabs.md).
