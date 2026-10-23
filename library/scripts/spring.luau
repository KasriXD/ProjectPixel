-- Spring: launches whatever lands on it upwards.
--
-- Attach to an entity with a trigger BoxCollider (platformer games).

local Spring = {
    properties = {
        strength = 420, -- upward speed given, units per second
    },
}

function Spring:TriggerEnter(other)
    if other:hasComponent("KinematicBody") then
        other.velocity = Vec2(other.velocity.x, -self.strength)
    end
end

return Spring
