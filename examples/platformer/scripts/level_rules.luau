-- Level rules: shows how many coins are left and completes the level when
-- every coin has been collected. Can also play music, a sound at the end
-- and show the score in a Text (the HUD).
--
-- Attach to any entity (it needs no Transform). Coins are found by tag and
-- report themselves through the "coin_collected" event. Sounds are named as
-- in the Assets panel.

local LevelRules = {
    properties = {
        tag = "coin", -- which entities count as coins
        label = "COINS", -- shown before the count on the HUD
        message = "ALL COINS COLLECTED!", -- shown when the level is complete
        complete = true, -- end the level when all are collected (off: an exit ends it)
        music = "", -- a sound played as music from the start (e.g. "Theme"); empty for none
        completeSound = "", -- a sound played when the level is complete; empty for none
        scoreText = "", -- the name of an entity with a Text that shows the score (e.g. "Score"); empty for none
    },
}

function LevelRules:Start()
    self.total = #World.findWithTag(self.tag)
    self.collected = 0
    self:showProgress()
    if self.music ~= "" then
        Sound.playMusic(self.music, 0.6)
    end

    Events.on("coin_collected", function()
        self.collected += 1
        self:showProgress()
        if self.complete and self.collected >= self.total then
            Events.emit("level_complete", self.message) -- for Best Time and other listeners
            Game.completeLevel(self.message)
        end
    end)
    Events.on("level_complete", function()
        if self.completeSound ~= "" then
            Sound.play(self.completeSound)
        end
    end)
end

function LevelRules:showProgress()
    Game.setObjective(string.format("%s %d/%d", self.label, self.collected, self.total))
    if self.scoreText ~= "" then
        local hud = World.find(self.scoreText)
        if hud then
            hud.text = "SCORE " .. Game.getScore()
        end
    end
end

return LevelRules
