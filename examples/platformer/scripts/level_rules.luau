-- Level rules: shows how many coins are left and completes the level when
-- every coin has been collected.
--
-- Attach to any entity (it needs no Transform). Coins are found by tag and
-- report themselves through the "coin_collected" event.

local LevelRules = {
    properties = {
        tag = "coin", -- which entities count as coins
        message = "ALL COINS COLLECTED!", -- shown when the level is complete
    },
}

function LevelRules:Start()
    self.total = #World.findWithTag(self.tag)
    self.collected = 0
    self:showProgress()

    Events.on("coin_collected", function()
        self.collected += 1
        self:showProgress()
        if self.collected >= self.total then
            Game.completeLevel(self.message)
        end
    end)
end

function LevelRules:showProgress()
    Game.setObjective(string.format("COINS %d/%d", self.collected, self.total))
end

return LevelRules
