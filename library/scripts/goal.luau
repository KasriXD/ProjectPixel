-- Goal: completes the level when the player reaches it.
--
-- Attach to an entity with a trigger BoxCollider: a flag, a door, stairs.

local Goal = {
    properties = {
        message = "LEVEL COMPLETE!", -- shown when the player arrives
        sound = "", -- a sound played when the player arrives (e.g. "Complete"); empty for none
    },
}

function Goal:TriggerEnter(other)
    if other:hasComponent("Player") then
        if self.sound ~= "" then
            Sound.play(self.sound)
        end
        Events.emit("level_complete", self.message) -- for Best Time and other listeners
        Game.completeLevel(self.message)
    end
end

return Goal
