-- Coin: awards points when the player touches it, then disappears. Each
-- player's lifetime coin count is saved ("coins"): on the platform it is
-- still there the next time they play.
--
-- Attach to an entity with a trigger BoxCollider and the "coin" tag.
-- Property values can be changed per coin in the scene file.

local Coin = {
    properties = {
        value = 1, -- points awarded when collected
        effect = "", -- name of a prefab to spawn where the coin was (e.g. "Sparkle"); empty for none
    },
}

function Coin:TriggerEnter(other)
    if not other:hasComponent("Player") then
        return
    end
    Game.addScore(self.value)
    Data.increment(other, "coins", self.value)
    Events.emit("coin_collected", self.value)
    if self.effect ~= "" then
        World.spawn(self.effect, self.entity.position)
    end
    self.entity:destroy()
end

return Coin
