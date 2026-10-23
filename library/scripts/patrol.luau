-- Patrol: walks back and forth between its start position and
-- start + offset at a steady speed, facing the way it is going.
--
-- Combine with Hazard to make an enemy. Uses simulation time, so every
-- patrol moves the same way on every machine.

local Patrol = {
    properties = {
        offset = Vec2(48, 0), -- how far it walks from where it starts
        period = 3, -- seconds for a full trip there and back
    },
}

function Patrol:Start()
    self.origin = self.entity.position
end

function Patrol:Tick(dt)
    local t = (Game.time() / self.period) % 1 -- 0 .. 1 over one round trip
    local returning = t >= 0.5
    local along = if returning then (1 - t) * 2 else t * 2
    self.entity.position = self.origin + self.offset * along
    if self.offset.x ~= 0 and self.entity:hasComponent("Sprite") then
        -- Face left when moving towards smaller x.
        self.entity.flipX = returning == (self.offset.x > 0)
    end
end

return Patrol
