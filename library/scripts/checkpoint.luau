-- Checkpoint: from now on the player respawns here instead of at the start.
--
-- Attach to an entity with a trigger BoxCollider.

local Checkpoint = {}

function Checkpoint:TriggerEnter(other)
    if other:hasComponent("Player") then
        other.spawnPoint = self.entity.position
    end
end

return Checkpoint
