-- Hazard: sends the player back to its spawn point (or the last checkpoint)
-- when it touches this entity.
--
-- Attach to an entity with a trigger BoxCollider: spikes, lava, enemies.

local Hazard = {}

function Hazard:TriggerEnter(other)
    if other:hasComponent("Player") then
        other:respawn()
    end
end

return Hazard
