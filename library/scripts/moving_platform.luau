-- Moving platform: glides back and forth between its start position and
-- start + offset. Bodies standing on it are carried along by the engine.
--
-- Attach to an entity with a solid (non-trigger) BoxCollider.

local MovingPlatform = {
    properties = {
        offset = Vec2(80, 0), -- how far it travels from its start position
        period = 4, -- seconds for a full trip there and back
    },
}

function MovingPlatform:Start()
    self.origin = self.entity.position
end

function MovingPlatform:Tick(dt)
    -- Smooth back-and-forth: 0 at the start, 1 at the far end. Game.time()
    -- is simulation time, so the motion is identical on every machine.
    local phase = (1 - math.cos(Game.time() / self.period * 2 * math.pi)) / 2
    self.entity.position = self.origin + self.offset * phase
end

return MovingPlatform
