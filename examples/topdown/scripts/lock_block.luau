-- Lock Block: a solid block that disappears when a Key with the same
-- keyName is collected.
--
-- Attach to an entity with a solid (not trigger) BoxCollider.

local LockBlock = {
    properties = {
        keyName = "gold", -- the key that opens it
    },
}

function LockBlock:Start()
    Events.on("key_collected", function(name)
        if name == self.keyName and self.entity:isValid() then
            self.entity:destroy()
        end
    end)
end

return LockBlock
