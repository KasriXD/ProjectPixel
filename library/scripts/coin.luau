-- Coin: awards points when the player touches it, then disappears.
--
-- Attach to an entity with a trigger BoxCollider and the "coin" tag.
-- Property values can be changed per coin in the scene file.

local Coin = {
    properties = {
        value = 1, -- points awarded when collected
    },
}

function Coin:TriggerEnter(other)
    if not other:hasComponent("Player") then
        return
    end
    Game.addScore(self.value)
    Events.emit("coin_collected", self.value)
    self.entity:destroy()
end

return Coin
