-- Unload Lua
-- Free memory used by the Lua module
menu.close()
console.show()
sleep(0.5)
io.write("Unload test...\n")
function my_task()
    io.write("User task here.\n")
    sleep(2)
    io.write("User task exiting.\n")
end
event.keypress = function(key)
    if key == KEY.PLAY then
        task.create(my_task)
        return false
    end
end
io.write("Press PLAY to start a new task (within the next 10 seconds).\n")
task.yield(10000)
event.keypress = nil
io.write("You can no longer start a new task with PLAY.\n")
io.write("Main task exiting.\n")
