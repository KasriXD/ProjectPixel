-- Sparkle: a short effect that rises, shrinks and disappears.
--
-- It lives in the "Sparkle" prefab; the Coin behaviour spawns it with
-- World.spawn when a coin is collected (its "effect" property).

local Sparkle = {
    properties = {
        lifetime = 0.35, -- seconds until it disappears
        rise = 30, -- upward speed, units per second
    },
}

function Sparkle:Start()
    self.age = 0
end

function Sparkle:Tick(dt)
    self.age += dt
    local t = self.age / self.lifetime
    if t >= 1 then
        self.entity:destroy()
        return
    end
    self.entity.position += Vec2(0, -self.rise * dt)
    local size = 1 - 0.6 * t
    self.entity.scale = Vec2(size, size)
end

return Sparkle
