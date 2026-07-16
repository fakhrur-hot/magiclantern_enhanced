keys = {}
keys.runnning = false
function keys:start()
    if keys.running then
        self:reset()
        return false
    end
    self.old_keypress = event.keypress
    self.keys = {}
    self.running = true
    event.keypress = function(key)
        if key ~= 0 then
            table.insert(keys.keys, key)
        end
        if key <= KEY.UNPRESS_FULLSHUTTER then
            return true
        end
        return false
    end
    return true
end
function keys:getkey()
    return table.remove(self.keys, 1)
end
function keys:reset()
    self.keys = {}
end
function keys:stop()
    self:reset()
    self.running = false
    event.keypress = self.old_keypress
end
function keys:anykey()
    local started = self:start()
    task.yield(100)
    self:reset()
    while true do
        local key = self:getkey()
        if key ~= nil then
            if key ~= KEY.UNPRESS_SET then
                break
            end
        end
        task.yield(100)
    end
    if started then self:stop() end
end
return keys
