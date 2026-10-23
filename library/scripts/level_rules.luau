-- Level rules: shows how many coins are left and completes the level when
-- every coin has been collected.
--
-- Attach to any entity (it needs no Transform). Coins are found by tag and
-- report themselves through the "coin_collected" event.

local LevelRules = {
    properties = {
        tag = "coin", -- which entities count as coins
        label = "COINS", -- shown before the count on the HUD
        message = "ALL COINS COLLECTED!", -- shown when the level is complete
        complete = true, -- end the level when all are collected (off: an exit ends it)
    },
}

function LevelRules:Start()
    self.total = #World.findWithTag(self.tag)
    self.collected = 0
    self:showProgress()

    Events.on("coin_collected", function()
        self.collected += 1
        self:showProgress()
        if self.complete and self.collected >= self.total then
            Game.completeLevel(self.message)
        end
    end)
end

function LevelRules:showProgress()
    Game.setObjective(string.format("%s %d/%d", self.label, self.collected, self.total))
end

return LevelRules
