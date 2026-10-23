-- Sign: shows a message on the HUD while the player stands at it.
--
-- Attach to an entity with a trigger BoxCollider.

local Sign = {
    properties = {
        message = "Hello! Collect everything you can.", -- what the sign says
    },
}

function Sign:TriggerEnter(other)
    if other:hasComponent("Player") then
        self.previous = Game.getObjective()
        Game.setObjective(self.message)
    end
end

function Sign:TriggerExit(other)
    if other:hasComponent("Player") and self.previous ~= nil then
        Game.setObjective(self.previous)
        self.previous = nil
    end
end

return Sign
