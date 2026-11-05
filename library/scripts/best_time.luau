-- Best time: when the level is complete, puts how long it took on a
-- leaderboard for every player. Declare the leaderboard in project.json
-- under "leaderboards", with "order": "lowest" and "format": "time"
-- (docs/leaderboards.md).
--
-- Attach to any entity, for example next to Level Rules. The level's end is
-- the "level_complete" event that Level Rules and Goal send.

local BestTime = {
    properties = {
        board = "fastest", -- the leaderboard's id in project.json
    },
}

function BestTime:Start()
    Events.on("level_complete", function()
        local seconds = Game.time()
        for _, player in Players.getAll() do
            Leaderboard.submit(player, self.board, seconds)
        end
    end)
end

return BestTime
