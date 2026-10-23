-- Goal: completes the level when the player reaches it.
--
-- Attach to an entity with a trigger BoxCollider: a flag, a door, stairs.

local Goal = {
    properties = {
        message = "LEVEL COMPLETE!", -- shown when the player arrives
    },
}

function Goal:TriggerEnter(other)
    if other:hasComponent("Player") then
        Game.completeLevel(self.message)
    end
end

return Goal
