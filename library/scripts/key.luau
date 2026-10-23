-- Key: picked up by the player. Opens every Lock Block with the same
-- keyName (they listen for the "key_collected" event).
--
-- Attach to an entity with a trigger BoxCollider.

local Key = {
    properties = {
        keyName = "gold", -- matches the keyName of the lock blocks it opens
    },
}

function Key:TriggerEnter(other)
    if not other:hasComponent("Player") then
        return
    end
    Events.emit("key_collected", self.keyName)
    self.entity:destroy()
end

return Key
