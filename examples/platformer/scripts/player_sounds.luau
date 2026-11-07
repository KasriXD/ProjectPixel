-- Player Sounds: plays a sound when the character jumps and when it lands.
--
-- Attach to the Player entity. Sounds are named as in the Assets panel; the
-- sounds come from the character, so others hear them from where it is.

local PlayerSounds = {
    properties = {
        jump = "", -- played when it jumps (e.g. "Jump"); empty for none
        land = "", -- played when it lands after a fall; empty for none
    },
}

function PlayerSounds:Start()
    self.lastSpeed = 0
    self.wasOnGround = true
end

function PlayerSounds:Tick()
    local speed = self.entity.velocity.y -- up is negative
    local onGround = self.entity.onGround
    -- A jump (or a spring) sends it up at once; gravity changes the speed slowly.
    if self.jump ~= "" and speed < -150 and speed < self.lastSpeed - 150 then
        self.entity:playSound(self.jump)
    end
    if self.land ~= "" and onGround and not self.wasOnGround and self.lastSpeed > 200 then
        self.entity:playSound(self.land, 0.6)
    end
    self.lastSpeed = speed
    self.wasOnGround = onGround
end

return PlayerSounds
