-- API Tests
-- Tests for the Lua scripting API
require("logger")
test_log = nil
function printf(s,...)
    test_log:writef(s,...)
    if not console.visible then
        display.notify_box(s:format(...), 5000)
    end
end
function alert()
    display.off()
    beep()
    display.on()
end
function request_mode(mode, mode_str)
    if camera.mode ~= mode or not camera.gui.idle then
        printf("Please switch to %s mode.\n", mode_str, mode)
        while camera.mode ~= mode or not camera.gui.idle do
            console.show(); assert(console.visible)
            if camera.gui.idle then alert() end
            sleep(1)
        end
    end
    sleep(2)
end
function round(x)
    return x + 0.5 - (x + 0.5) % 1
end
function print_table(t)
    test_log:write(string.format("%s = ", t))
    local s,e = xpcall(function()
        test_log:serialize(_G[t])
    end, debug.traceback)
    if s == false then
        test_log:write(e)
    end
end
declared_var = nil
function strict_tests()
    printf("Strict mode tests...\n")
    declared_var = 5
    assert(declared_var == 5)
    local s,e = pcall(function() undeclared_var = 7 end)
    assert(s == false)
    assert(e:find("assign to undeclared variable 'undeclared_var'"))
    local s,e = pcall(function() print(undeclared_var) end)
    assert(s == false)
    assert(e:find("variable 'undeclared_var' is not declared"))
    printf("Strict mode tests passed.\n")
    printf("\n")
end
function generic_tests()
    printf("Generic tests...\n")
    print_table("arg")
    print_table("camera")
    print_table("event")
    print_table("console")
    print_table("lv")
    print_table("lens")
    print_table("display")
    print_table("key")
    print_table("menu")
    print_table("movie")
    print_table("dryos")
    print_table("interval")
    print_table("battery")
    print_table("task")
    print_table("property")
    printf("Generic tests completed.\n")
    printf("\n")
end
function stdio_test()
    local s,e
    io.write("Hello, stdout. ")
    io.stderr:write("Hello, stderr. ")
    io.write("\n")
    io.write("Hello again, stdout.\n")
    io.stderr:write("Hello again, stderr.\n")
    local inp = io.read("*line")
    print("From stdin:", inp)
end
function copy_test(src, dst)
    printf("Copy test: %s -> %s\n", src, dst)
    local fin = io.open(src, "rb")
    local data = fin:read("*all")
    fin:close()
    local fout = io.open(dst, "w")
    fout:write(data)
    fout:close()
    fin = io.open(dst, "rb")
    local check = fin:read("*all")
    local size = fin:seek("end", 0)
    fin:close()
    assert(data == check)
    assert(size == #data)
    assert(dryos.remove(dst) == true)
    assert(io.open(dst, "rb") == nil)
    assert(dryos.remove(dst) == false)
    printf("Copy test OK\n")
end
function append_test(file)
    printf("Append test: %s\n", file)
    local data1 = "Allons enfants de la patrie\n"
    local data2 = "Le jour de gloire est arrivé !\n"
    dryos.remove(file)
    local fout = io.open(file, "a")
    fout:write(data1)
    fout:close()
    local fin = io.open(file, "r")
    local check = fin:read("*all")
    fin:close()
    assert(check == data1)
    fout = io.open(file, "a")
    fout:write(data2)
    fout:close()
    fin = io.open(file, "r")
    check = fin:read("*all")
    fin:close()
    assert(check == data1 .. data2)
    assert(dryos.remove(file) == true)
    assert(io.open(file, "rb") == nil)
    printf("Append test OK\n")
end
function rename_test(src, dst)
    printf("Rename test: %s -> %s\n", src, dst)
    local data = "Millions saw the apple fall, " ..
        "but Newton was the one who asked why."
    local fout = io.open(src, "w")
    fout:write(data)
    fout:close()
    local fin = io.open(src, "r")
    local check = fin:read("*all")
    fin:close()
    assert(check == data)
    assert(dryos.rename(src, dst) == true)
    fin = io.open(dst, "r")
    check = fin:read("*all")
    fin:close()
    assert(check == data)
    assert(io.open(src, "rb") == nil)
    assert(dryos.remove(dst) == true)
    assert(io.open(dst, "rb") == nil)
    printf("Rename test OK\n")
end
function list_dir(dir, prefix)
    prefix = prefix or "- "
    local i, d, f
    for i,d in pairs(dir:children()) do
       printf("%s%s\n", prefix, d)
       list_dir(d, "  " .. prefix)
    end
    for i,f in pairs(dir:files()) do
       printf("%s%s\n", prefix, f)
    end
end
function card_test()
    if dryos.cf_card then
        printf("CF card (%s) present\n", dryos.cf_card.path)
        printf("- free space: %d MiB\n", dryos.cf_card.free_space)
        printf("- next image: %s\n", dryos.cf_card:image_path(1))
        printf("- DCIM dir. : %s\n", dryos.cf_card.dcim_dir)
        assert(dryos.cf_card.path == "A:/")
        assert(dryos.cf_card.type == "CF")
        list_dir(dryos.cf_card.dcim_dir)
        list_dir(dryos.directory(dryos.cf_card.path))
    end
    if dryos.sd_card then
        printf("SD card (%s) present\n", dryos.sd_card.path)
        printf("- free space: %d MiB\n", dryos.sd_card.free_space)
        printf("- next image: %s\n", dryos.sd_card:image_path(1))
        printf("- DCIM dir. : %s\n", dryos.sd_card.dcim_dir)
        assert(dryos.sd_card.path == "B:/")
        assert(dryos.sd_card.type == "SD")
        list_dir(dryos.sd_card.dcim_dir)
        list_dir(dryos.directory(dryos.sd_card.path))
    end
end
function test_io()
    printf("Testing file I/O...\n")
    stdio_test()
    copy_test("autoexec.bin", "tmp.bin")
    append_test("tmp.txt")
    rename_test("apple.txt", "banana.txt")
    rename_test("apple.txt", "ML/banana.txt")
    card_test()
    printf("File I/O tests completed.\n")
    printf("\n")
end
function test_camera_gui()
    printf("Testing Canon GUI functions...\n")
    camera.gui.play = true
    assert(camera.gui.play == true)
    assert(camera.gui.mode == 1)
    key.press(KEY.HALFSHUTTER)
    sleep(1)
    assert(camera.gui.play == false)
    assert(camera.gui.mode == 0)
    key.press(KEY.UNPRESS_HALFSHUTTER)
    for i = 1,100 do
        if math.random(1,2) == 1 then
            if math.random(1,2) == 1 then
                printf("Enter PLAY mode...\n");
                camera.gui.play = true
                assert(camera.gui.play == true)
                assert(camera.gui.menu == false)
                assert(camera.gui.idle == false)
                assert(camera.gui.mode == 1)
            elseif camera.gui.play then
                printf("Exit PLAY mode...\n");
                camera.gui.play = false
                assert(camera.gui.play == false)
                assert(camera.gui.menu == false)
                assert(camera.gui.idle == true)
                assert(camera.gui.mode == 0)
            end
        else
            if math.random(1,2) == 1 then
                printf("Enter MENU mode...\n");
                camera.gui.menu = true
                assert(camera.gui.menu == true)
                assert(camera.gui.play == false)
                assert(camera.gui.idle == false)
                assert(camera.gui.mode == 2)
            elseif camera.gui.menu then
                printf("Exit MENU mode...\n");
                camera.gui.menu = false
                assert(camera.gui.menu == false)
                assert(camera.gui.play == false)
                assert(camera.gui.idle == true)
                assert(camera.gui.mode == 0)
            end
        end
        if camera.gui.menu == false and camera.gui.play == false then
            if math.random(1,2) == 1 then
                if not lv.enabled then
                    printf("Start LiveView...\n");
                    lv.start()
                elseif lv.paused then
                    printf("Resume LiveView...\n");
                    lv.resume()
                elseif math.random(1,10) < 9 then
                    printf("Pause LiveView...\n");
                    lv.pause()
                else
                    printf("Stop LiveView...\n");
                    lv.stop()
                end
            end
        end
    end
    lv.stop()
    assert(not lv.enabled)
    printf("Canon GUI tests completed.\n")
    printf("\n")
end
function test_menu()
    printf("Testing ML menu API...\n")
    menu.open()
    assert(menu.select("Expo", "ISO"))
    assert(menu.set("Expo", "ISO", "200"))
    assert(camera.iso.value == 200)
    sleep(1)
    assert(menu.set("Expo", "ISO", 1600))
    assert(camera.iso.value == 1600)
    sleep(1)
    assert(menu.select("Expo", "Picture Style"))
    assert(menu.set("Expo", "Picture Style", "Portrait"))
    assert(menu.get("Expo", "Picture Style") == "Portrait")
    sleep(1)
    assert(menu.set("Expo", "Picture Style", 5))
    sleep(1)
    assert(menu.set("Expo", "Picture Style", "Landscape"))
    assert(menu.get("Expo", "Picture Style") == "Landscape")
    sleep(1)
    assert(menu.set("Expo", "Picture Style", 1234) == false)
    assert(menu.get("Expo", "Picture Style") == "Landscape")
    sleep(1)
    assert(menu.select("Overlay"))
    assert(menu.select("Movie"))
    if menu.get("Movie", "FPS override") ~= nil then
        menu.close()
        lv.start()
        assert(lv.enabled)
        assert(lv.running)
        menu.open()
        assert(menu.select("Movie", "FPS override"))
        assert(menu.set("Movie", "FPS override", "OFF"))
        assert(menu.get("Movie", "FPS override") == "OFF")
        assert(menu.set("FPS override", "Desired FPS", "23.976"))
        assert(menu.get("FPS override", "Desired FPS"):sub(1,13) == "23.976 (from ")
        assert(menu.set("FPS override", "Desired FPS", "5"))
        assert(menu.get("FPS override", "Desired FPS"):sub(1,8) == "5 (from ")
        assert(menu.set("FPS override", "Desired FPS", "10"))
        assert(menu.get("FPS override", "Desired FPS"):sub(1,9) == "10 (from ")
        assert(menu.set("FPS override", "Optimize for", "Exact FPS"))
        assert(menu.get("FPS override", "Optimize for") == "Exact FPS")
        assert(menu.set("Movie", "FPS override", "ON"))
        assert(menu.get("Movie", "FPS override") ~= "ON")
        sleep(2)
        assert(menu.get("Movie", "FPS override") == "10.000")
        assert(menu.get("FPS override", "Actual FPS") == "10.000")
        assert(menu.set("Movie", "FPS override", "OFF"))
        assert(menu.get("Movie", "FPS override") == "OFF")
        menu.close()
        lv.stop()
        assert(not lv.running)
        assert(not lv.enabled)
        menu.open()
    end
    assert(menu.select("Shoot"))
    assert(menu.select("Shoot", "Advanced Bracket"))
    assert(menu.set("Shoot", "Advanced Bracket", 1))
    assert(menu.get("Shoot", "Advanced Bracket", 0) == 1)
    sleep(1)
    assert(menu.set("Shoot", "Intervalometer", "ON"))
    assert(menu.get("Shoot", "Intervalometer", 0) == 1)
    sleep(1)
    assert(menu.set("Shoot", "Advanced Bracket", "OFF"))
    assert(menu.get("Shoot", "Advanced Bracket", 0) == 0)
    assert(menu.get("Shoot", "Advanced Bracket") == "OFF")
    sleep(1)
    assert(menu.set("Shoot", "Advanced Bracket", "ON"))
    assert(menu.get("Shoot", "Advanced Bracket") ~= "ON")
    assert(menu.get("Shoot", "Advanced Bracket") ~= "OFF")
    assert(menu.set("Shoot", "Advanced Bracket", "OFF"))
    assert(menu.get("Shoot", "Advanced Bracket") == "OFF")
    assert(menu.set("Shoot", "Intervalometer", 0))
    assert(menu.get("Shoot", "Intervalometer", 0) == 0)
    assert(menu.get("Shoot", "Intervalometer") == "OFF")
    sleep(1)
    assert(menu.select("Shoot", "Intervalometer"))
    sleep(1)
    assert(menu.select("Intervalometer", "Take a pic every"))
    sleep(1)
    assert(menu.set("Intervalometer", "Take a pic every", "1m10s"))
    assert(menu.get("Intervalometer", "Take a pic every", 0) == 70)
    assert(menu.get("Intervalometer", "Take a pic every") == "1m10s")
    assert(menu.set("Intervalometer", "Take a pic every", "1m30s"))
    assert(menu.get("Intervalometer", "Take a pic every", 0) == 90)
    assert(menu.get("Intervalometer", "Take a pic every") == "1m30s")
    assert(menu.set("Intervalometer", "Take a pic every", "1m10s"))
    assert(menu.get("Intervalometer", "Take a pic every", 0) == 70)
    assert(menu.get("Intervalometer", "Take a pic every") == "1m10s")
    sleep(1)
    assert(menu.set("Intervalometer", "Take a pic every", "10"))
    assert(menu.get("Intervalometer", "Take a pic every", 0) == 10)
    assert(menu.get("Intervalometer", "Take a pic every") == "10s")
    sleep(1)
    assert(menu.set("Intervalometer", "Take a pic every", 70))
    assert(menu.get("Intervalometer", "Take a pic every", 0) == 70)
    assert(menu.get("Intervalometer", "Take a pic every") == "1m10s")
    sleep(1)
    assert(menu.set("Intervalometer", "Take a pic every", 7000000) == false)
    assert(menu.get("Intervalometer", "Take a pic every", 0) == 70)
    assert(menu.get("Intervalometer", "Take a pic every") == "1m10s")
    sleep(1)
    assert(menu.select("Shoot", "Intervalometer"))
    sleep(1)
    assert(menu.select("Advanced Bracket", "Frames")); sleep(1)
    assert(menu.select("Advanced Bracket", "Sequence")); sleep(1)
    assert(menu.select("Advanced Bracket", "ISO shifting")); sleep(1)
    assert(menu.select("Bulb Timer", "Exposure duration")); sleep(1)
    assert(menu.select("Shoot Preferences", "Snap Simulation")); sleep(1)
    assert(menu.select("Misc key settings", "Sticky HalfShutter")); sleep(1)
    assert(menu.select("LiveView zoom tweaks", "Zoom on HalfShutter")); sleep(1)
    assert(menu.select("Lens info", "Lens ID")); sleep(1)
    assert(menu.select("Shoot", "Intervalometer")); sleep(1)
    assert(menu.set("Shoot", "Intervalometer", "ON")); sleep(1)
    assert(menu.select("Modified", "Intervalometer")); sleep(1)
    assert(menu.select("Modified", "Take a pic every")); sleep(1)
    assert(menu.select("Modified", "Intervalometer")); sleep(1)
    assert(menu.set("Shoot", "Intervalometer", "OFF")); sleep(1)
    assert(menu.select("Modified", "Take a pic every") == false); sleep(1)
    assert(menu.set("Shoot", "Intervalometer", "ON")); sleep(1)
    assert(menu.select("Modified", "Take a pic every")); sleep(1)
    assert(menu.set("Intervalometer", "Take a pic every", 10)); sleep(1)
    assert(menu.select("Modified", "Intervalometer")); sleep(1)
    assert(menu.set("Shoot", "Intervalometer", "OFF")); sleep(1)
    assert(menu.select("Modified", "Take a pic every") == false); sleep(1)
    assert(menu.select("Shoot", "Intervalometer")); sleep(1)
    assert(menu.select("Intervalometer", "Take a pic every")); sleep(1)
    assert(menu.select("Intervalometer", "Start trigger")); sleep(1)
    assert(menu.set("Intervalometer", "Start trigger", "Leave Menu")); sleep(0.5);
    assert(menu.select("Intervalometer", "Start after")); sleep(0.5)
    assert(menu.set("Intervalometer", "Start after", "3s")); sleep(0.5);
    assert(menu.select("Intervalometer", "Stop after")); sleep(0.5)
    assert(menu.set("Intervalometer", "Stop after", "Disabled")); sleep(0.5);
    assert(menu.select("Shoot", "Intervalometer")); sleep(1)
    assert(menu.select("Modified", "Intervalometer") == false); sleep(1)
    assert(menu.select("Dinosaur") == false)
    assert(menu.select("Shoot", "Crocodile") == false)
    assert(menu.get("Shoot", "Introvolometer", 0) == nil)
    assert(menu.get("Shoot", "Brack") == nil)
    assert(menu.set("Shoot", "Introvolometer", 1) == nil)
    assert(menu.set("Shoot", "Introvolometer", "OFF") == nil)
    menu.close()
    for i = 1,5 do
        menu.open()
        assert(camera.gui.idle == false)
        menu.close()
        assert(camera.gui.idle == true)
    end
    printf("Menu tests completed.\n")
    printf("\n")
end
function taskA()
    printf("Task A started.\n")
    local i
    for i = 1,100 do
        printf("Task A: %d\n", i)
        task.yield(math.random(10,50))
    end
end
function taskB()
    printf("Task B started.\n")
    local i
    for i = 1,100 do
        printf("Task B: %d\n", i)
        task.yield(math.random(10,50))
    end
end
function taskC()
    printf("Task C started.\n")
    msleep(math.random(10,50))
    printf("Task C finished.\n")
end
function taskD()
    io.write("Task D started.\n")
    msleep(math.random(10,50))
    io.write("Task D finished.\n")
end
function test_multitasking()
    printf("Testing multitasking...\n")
    printf("Only one task allowed to interrupt...\n")
    for i = 1,10 do
        task.create(taskC)
        printf("Main task yielding.\n")
        task.yield(math.random(10,50))
        printf("Main task back.\n")
    end
    for i = 1,1000 do
        task.create(taskD)
        io.write("Main task yielding.\n")
        task.yield(math.random(10,50))
        io.write("Main task back.\n")
    end
    task.yield(500)
    printf("Multitasking tests completed.\n")
    printf("\n")
end
function test_keys()
    printf("Testing half-shutter...\n")
    for i = 1,10 do
        camera.gui.menu = true
        sleep(1)
        assert(camera.gui.menu == true)
        assert(camera.gui.idle == false)
        key.press(KEY.HALFSHUTTER)
        sleep(0.2)
        if key.last ~= KEY.HALFSHUTTER then
            printf("warning: last key not half-shutter, but %d\n", key.last)
        end
        sleep(1)
        assert(camera.gui.menu == false)
        assert(camera.gui.idle == true)
        key.press(KEY.UNPRESS_HALFSHUTTER)
        sleep(0.2)
        if key.last ~= KEY.UNPRESS_HALFSHUTTER then
            printf("warning: last key not unpress half-shutter, but %d\n", key.last)
        end
    end
    printf("Half-shutter test OK.\n")
    printf("\n")
end
function test_camera_exposure()
    printf("Testing exposure settings...\n")
    printf("Camera    : %s (%s) %s\n", camera.model, camera.model_short, camera.firmware)
    printf("Lens      : %s\n", lens.name)
    printf("Shoot mode: %s\n", camera.mode)
    printf("Shutter   : %s (raw %s, %ss, %sms, apex %s)\n", camera.shutter, camera.shutter.raw, camera.shutter.value, camera.shutter.ms, camera.shutter.apex)
    printf("Aperture  : %s (raw %s, f/%s, apex %s)\n", camera.aperture, camera.aperture.raw, camera.aperture.value, camera.aperture.apex)
    printf("Av range  : %s..%s (raw %s..%s, f/%s..f/%s, apex %s..%s)\n",
        camera.aperture.min, camera.aperture.max,
        camera.aperture.min.raw, camera.aperture.max.raw,
        camera.aperture.min.value, camera.aperture.max.value,
        camera.aperture.min.apex, camera.aperture.max.apex
    )
    printf("ISO       : %s (raw %s, %s, apex %s)\n", camera.iso, camera.iso.raw, camera.iso.value, camera.iso.apex)
    printf("EC        : %s (raw %s, %s EV)\n", camera.ec, camera.ec.raw, camera.ec.value)
    printf("Flash EC  : %s (raw %s, %s EV)\n", camera.flash_ec, camera.flash_ec.raw, camera.flash_ec.value)
    request_mode(MODE.M, "M")
    local old_value = camera.shutter.raw
    printf("Setting shutter to random values...\n")
    for k = 1,100 do
        local method = math.random(1,4)
        local d = nil
        if method == 1 then
            local s = math.random(1,30)
            if math.random(1,2) == 1 then
                camera.shutter.value = s
            else
                camera.shutter = s
            end
            d = math.abs(math.log(camera.shutter.value,2) - math.log(s,2))
        elseif method == 2 then
            local ms = math.random(1,30000)
            camera.shutter.ms = ms
            d = math.abs(math.log(camera.shutter.ms,2) - math.log(ms,2))
        elseif method == 3 then
            local apex = math.random(-5*100,12*100)/100
            camera.shutter.apex = apex
            d = math.abs(camera.shutter.apex - apex)
        elseif method == 4 then
            local raw = math.random(16,152)
            camera.shutter.raw = raw
            d = math.abs(camera.shutter.raw - raw) / 8
        end
        if d > 1.5/8 + 1e-3 then
            printf("Error: shutter delta %s EV\n", d)
        end
        if math.abs(camera.shutter.value - camera.shutter.ms/1000) > 1e-3 then
            printf("Error: shutter %ss != %sms\n", camera.shutter.value, camera.shutter.ms)
        end
        local expected_apex = math.log(1/camera.shutter.value,2)
        if math.abs(expected_apex - camera.shutter.apex) > 0.01 then
            printf("Error: shutter %ss != Tv%s, expected %s\n", camera.shutter.value, camera.shutter.apex, expected_apex)
        end
        for i,field in pairs{"value","ms","apex","raw"} do
            local current = {}
            current.apex  = camera.shutter.apex
            current.value = camera.shutter.value
            current.ms    = camera.shutter.ms
            current.raw   = camera.shutter.raw
            if field == "ms" and current.ms < 10 and method ~= 2 then
                field = "value"
            end
            camera.shutter[field] = current[field]
            if camera.shutter.value ~= current.value then
                printf("Error: shutter set to %s=%s, got %ss, expected %ss\n", field, current[field], camera.shutter.value, current.value)
            end
            if camera.shutter.ms ~= current.ms then
                printf("Error: shutter set to %s=%s, got %sms, expected %sms\n", field, current[field], camera.shutter.ms, current.ms)
            end
            if camera.shutter.apex ~= current.apex then
                printf("Error: shutter set to %s=%s, got Tv%s, expected Tv%s\n", field, current[field], camera.shutter.apex, current.apex)
            end
            if camera.shutter.raw ~= current.raw then
                printf("Error: shutter set to %s=%s, got %s, expected %s (raw)\n", field, current[field], camera.shutter.raw, current.raw)
            end
        end
    end
    camera.shutter.raw = old_value
    request_mode(MODE.M, "M")
    old_value = camera.iso.raw
    printf("Setting ISO to random values...\n")
    for k = 1,100 do
        local method = math.random(1,3)
        local d = nil
        if method == 1 then
            local iso = math.random(100, 3200)
            if math.random(1,2) == 1 then
                camera.iso.value = iso
            else
                camera.iso = iso
            end
            d = math.abs(math.log(camera.iso.value,2) - math.log(iso,2))
        elseif method == 2 then
            local apex = math.random(5*100,10*100)/100
            camera.iso.apex = apex
            d = math.abs(camera.iso.apex - apex)
        elseif method == 3 then
            local raw = math.random(72, 112)
            camera.iso.raw = raw
            d = math.abs(camera.iso.raw - raw) / 8
        end
        if d > 1/3 then
            printf("Error: ISO delta %s EV\n", d)
        end
        local expected_apex = math.log(camera.iso.value/3.125, 2)
        if math.abs(expected_apex - camera.iso.apex) > 0.2 then
            printf("Error: ISO %s != Sv%s, expected %s\n", camera.iso.value, camera.iso.apex, expected_apex)
        end
        for i,field in pairs{"value","apex","raw"} do
            local current = {}
            current.apex  = camera.iso.apex
            current.value = camera.iso.value
            current.raw   = camera.iso.raw
            camera.iso[field] = current[field]
            if camera.iso.value ~= current.value then
                printf("Error: ISO set to %s=%s, got %s, expected %s\n", field, current[field], camera.iso.value, current.value)
            end
            if camera.iso.apex ~= current.apex then
                printf("Error: ISO set to %s=%s, got Sv%s, expected Sv%s\n", field, current[field], camera.iso.apex, current.apex)
            end
            if camera.iso.raw ~= current.raw then
                printf("Error: ISO set to %s=%s, got %s, expected %s (raw)\n", field, current[field], camera.iso.raw, current.raw)
            end
        end
    end
    camera.iso.raw = old_value
    if camera.aperture.min.raw == camera.aperture.max.raw then
        printf("This lens does not have variable aperture (skipping test).\n")
    else
        request_mode(MODE.M, "M")
        old_value = camera.aperture.raw
        printf("Setting aperture to random values...\n")
        for k = 1,100 do
            local method = math.random(1,3)
            local d = nil
            local extra_tol = 0
            if method == 1 then
                local av = math.random(round(camera.aperture.min.value*10), round(camera.aperture.max.value*10)) / 10
                if math.random(1,2) == 1 then
                    camera.aperture.value = av
                else
                    camera.aperture = av
                end
                d = math.abs(math.log(camera.aperture.value,2) - math.log(av,2)) * 2
                extra_tol = math.abs(math.log(av,2) - math.log(av-0.1,2)) * 2
            elseif method == 2 then
                local apex = math.random(round(camera.aperture.min.apex*100), round(camera.aperture.max.apex*100)) / 100
                camera.aperture.apex = apex
                d = math.abs(camera.aperture.apex - apex)
            elseif method == 3 then
                local raw = math.random(camera.aperture.min.raw, camera.aperture.max.raw)
                camera.aperture.raw = raw
                d = math.abs(camera.aperture.raw - raw) / 8
            end
            if (d > 1.5/8 + extra_tol) then
                printf("Error: aperture delta %s EV (expected < %s, %s, method=%d)\n", d, 1.5/8 + extra_tol, camera.aperture, method)
            end
            local expected_apex = math.log(camera.aperture.value, 2) * 2
            if math.abs(expected_apex - camera.aperture.apex) > 0.2 then
                printf("Error: aperture %s != Av%s, expected %s\n", camera.aperture.value, camera.aperture.apex, expected_apex)
            end
            for i,field in pairs{"value","apex","raw"} do
                local current = {}
                current.apex  = camera.aperture.apex
                current.value = camera.aperture.value
                current.raw   = camera.aperture.raw
                camera.aperture[field] = current[field]
                if camera.aperture.value ~= current.value then
                    printf("Error: aperture set to %s=%s, got %s, expected %s\n", field, current[field], camera.aperture.value, current.value)
                end
                if camera.aperture.apex ~= current.apex then
                    printf("Error: aperture set to %s=%s, got Sv%s, expected Sv%s\n", field, current[field], camera.aperture.apex, current.apex)
                end
                if camera.aperture.raw ~= current.raw then
                    printf("Error: aperture set to %s=%s, got %s, expected %s (raw)\n", field, current[field], camera.aperture.raw, current.raw)
                end
            end
        end
        camera.aperture.raw = old_value
    end
    request_mode(MODE.AV, "Av")
    old_value = camera.ec.raw
    printf("Setting EC to random values...\n")
    for k = 1,100 do
        local method = math.random(1,2)
        local d = nil
        if method == 1 then
            local ec = math.random(-2*100, 2*100) / 100
            if math.random(1,2) == 1 then
                camera.ec.value = ec
            else
                camera.ec = ec
            end
            d = math.abs(camera.ec.value - ec,2)
        elseif method == 2 then
            local raw = math.random(-16, 16)
            camera.ec.raw = raw
            d = math.abs(camera.ec.raw - raw) / 8
        end
        if d > 1.5/8 then
            printf("Error: EC delta %s EV\n", d)
        end
        local expected_ec = camera.ec.raw / 8
        if math.abs(expected_ec - camera.ec.value) > 0.001 then
            printf("Error: EC raw %s != %s EV, expected %s EV\n", camera.ec.raw, camera.ec.value, expected_ec)
        end
        for i,field in pairs{"value","raw"} do
            local current = {}
            current.value = camera.ec.value
            current.raw   = camera.ec.raw
            camera.ec[field] = current[field]
            if camera.ec.value ~= current.value then
                printf("Error: EC set to %s=%s, got %s, expected %s EV\n", field, current[field], camera.ec.value, current.value)
            end
            if camera.ec.raw ~= current.raw then
                printf("Error: EC set to %s=%s, got %s, expected %s (raw)\n", field, current[field], camera.ec.raw, current.raw)
            end
        end
    end
    camera.ec.raw = old_value
    old_value = camera.flash_ec.raw
    printf("Setting Flash EC to random values...\n")
    for k = 1,100 do
        local method = math.random(1,2)
        local d = nil
        if method == 1 then
            local fec = math.random(-2*100, 2*100) / 100
            if math.random(1,2) == 1 then
                camera.flash_ec.value = fec
            else
                camera.flash_ec = fec
            end
            d = math.abs(camera.flash_ec.value - fec,2)
        elseif method == 2 then
            local raw = math.random(-16, 16)
            camera.flash_ec.raw = raw
            d = math.abs(camera.flash_ec.raw - raw) / 8
        end
        if d > 1.5/8 then
            printf("Error: FEC delta %s EV\n", d)
        end
        local expected_fec = camera.flash_ec.raw / 8
        if math.abs(expected_fec - camera.flash_ec.value) > 0.001 then
            printf("Error: FEC raw %s != %s EV, expected %s EV\n", camera.flash_ec.raw, camera.flash_ec.value, current.expected_fec)
        end
        for i,field in pairs{"value","raw"} do
            local current = {}
            current.value = camera.flash_ec.value
            current.raw   = camera.flash_ec.raw
            camera.flash_ec[field] = current[field]
            if camera.flash_ec.value ~= current.value then
                printf("Error: FEC set to %s=%s, got %s, expected %s EV\n", field, current[field], camera.flash_ec.value, current.value)
            end
            if camera.flash_ec.raw ~= current.raw then
                printf("Error: FEC set to %s=%s, got %s, expected %s (raw)\n", field, current[field], camera.flash_ec.raw, current.raw)
            end
        end
    end
    camera.flash_ec.raw = old_value
    printf("Exposure tests completed.\n")
    printf("\n")
end
function print_file_size(filename)
    local f = io.open(filename, "rb")
    if f then
        local size = f:seek("end", 0)
        f:close()
        printf("%s: %s\n", filename, size);
        return size
    else
        printf("%s not found.\n", filename);
    end
end
function test_camera_take_pics()
    printf("Testing picture taking functions...\n")
    request_mode(MODE.M, "M")
    camera.shutter = 1/50
    sleep(2)
    printf("Snap simulation test...\n")
    assert(menu.set("Shoot Preferences", "Snap Simulation", 1))
    local initial_file_num = dryos.shooting_card.file_number
    camera.shoot()
    assert(dryos.shooting_card.file_number == initial_file_num)
    assert(menu.set("Shoot Preferences", "Snap Simulation", 0))
    sleep(2)
    printf("Single picture...\n")
    initial_file_num = dryos.shooting_card.file_number
    local image_path_cr2 = dryos.shooting_card:image_path(1, ".CR2")
    local image_path_jpg = dryos.shooting_card:image_path(1, ".JPG")
    local image_path_auto = dryos.shooting_card:image_path(1)
    assert(dryos.shooting_card:image_path(1, nil) == image_path_auto)
    assert(image_path_auto == image_path_cr2 or image_path_auto == image_path_jpg)
    assert(dryos.shooting_card:image_path(1, "") .. ".CR2" == image_path_cr2)
    assert(dryos.shooting_card:image_path(1, "") .. ".JPG" == image_path_jpg)
    assert(io.open(image_path_cr2, "rb") == nil)
    assert(io.open(image_path_jpg, "rb") == nil)
    camera.shoot()
    assert(dryos.shooting_card:image_path(0) == image_path_auto)
    assert(dryos.shooting_card:image_path() == image_path_auto)
    assert(dryos.shooting_card.dcim_dir.path == tostring(dryos.shooting_card.dcim_dir))
    local image_path_dcim =
        dryos.shooting_card.dcim_dir.path ..
        dryos.image_prefix ..
        string.format("%04d", dryos.shooting_card.file_number)
    assert(image_path_dcim == dryos.shooting_card:image_path(0, ""))
    assert((dryos.shooting_card.file_number - initial_file_num) % 9999 == 1)
    camera.wait()
    local size_cr2 = print_file_size(image_path_cr2)
    local size_jpg = print_file_size(image_path_jpg)
    assert(size_cr2 or size_jpg)
    assert(camera.gui.qr or camera.gui.idle)
    camera.gui.play = true
    assert(camera.gui.play == true)
    assert(camera.gui.play_photo == true)
    assert(camera.gui.play_movie == false)
    sleep(2)
    printf("Two burst pictures...\n")
    printf("Ideally, the camera should be in some continuous shooting mode (not checked).\n")
    initial_file_num = dryos.shooting_card.file_number
    local old_prefix = dryos.image_prefix
    dryos.image_prefix = "ABC_"
    assert(dryos.image_prefix == "ABC_")
    local image1_path_cr2 = dryos.shooting_card:image_path(1, ".CR2")
    local image1_path_jpg = dryos.shooting_card:image_path(1, ".JPG")
    local image2_path_cr2 = dryos.shooting_card:image_path(2, ".CR2")
    local image2_path_jpg = dryos.shooting_card:image_path(2, ".JPG")
    assert(io.open(image1_path_cr2, "rb") == nil)
    assert(io.open(image1_path_jpg, "rb") == nil)
    assert(io.open(image2_path_cr2, "rb") == nil)
    assert(io.open(image2_path_jpg, "rb") == nil)
    camera.burst(2)
    assert((dryos.shooting_card.file_number - initial_file_num) % 9999 == 2)
    camera.wait()
    local size1_cr2 = print_file_size(image1_path_cr2)
    local size1_jpg = print_file_size(image1_path_jpg)
    local size2_cr2 = print_file_size(image2_path_cr2)
    local size2_jpg = print_file_size(image2_path_jpg)
    assert(size1_cr2 or size1_jpg)
    assert(size2_cr2 or size2_jpg)
    dryos.image_prefix = ""
    assert(dryos.image_prefix == old_prefix)
    dryos.image_prefix = "XYZ_"
    assert(dryos.image_prefix == "XYZ_")
    dryos.image_prefix = "1234"
    assert(dryos.image_prefix == "1234")
    dryos.image_prefix = "$#@!"
    assert(dryos.image_prefix == "$#@!")
    dryos.image_prefix = ""
    assert(dryos.image_prefix == old_prefix)
    printf("Bracketed pictures...\n")
    initial_file_num = dryos.shooting_card.file_number
    camera.shutter.value = 1/500
    camera.shoot()
    camera.shutter.value = 1/50
    camera.shoot()
    camera.shutter.value = 1/5
    camera.shoot()
    assert((dryos.shooting_card.file_number - initial_file_num) % 9999 == 3)
    camera.wait()
    for i = -2,0 do
        image_path_cr2 = dryos.shooting_card:image_path(i, ".CR2")
        image_path_jpg = dryos.shooting_card:image_path(i, ".JPG")
        local size_cr2 = print_file_size(image_path_cr2)
        local size_jpg = print_file_size(image_path_jpg)
        assert(size_cr2 or size_jpg)
    end
    sleep(2)
    printf("Bulb picture...\n")
    local t0 = dryos.ms_clock
    initial_file_num = dryos.shooting_card.file_number
    image_path_cr2 = dryos.shooting_card:image_path(1, ".CR2")
    image_path_jpg = dryos.shooting_card:image_path(1, ".JPG")
    assert(io.open(image_path_cr2, "rb") == nil)
    assert(io.open(image_path_jpg, "rb") == nil)
    camera.bulb(10)
    local t1 = dryos.ms_clock
    local elapsed = t1 - t0
    printf("Elapsed time: %s\n", elapsed)
    assert(elapsed > 9900 and elapsed < 30000)
    assert((dryos.shooting_card.file_number - initial_file_num) % 9999 == 1)
    camera.wait()
    local size_cr2 = print_file_size(image_path_cr2)
    local size_jpg = print_file_size(image_path_jpg)
    assert(size_cr2 or size_jpg)
    printf("Picture taking tests completed.\n")
    printf("\n")
    sleep(5)
end
function test_ml_overlays()
    assert(lv.overlays == 2)
    local all_overlays = {"Zebras", "Focus Peak", "Magic Zoom",
        "Cropmarks", "Ghost image", "Spotmeter", "False color",
        "Histogram", "Waveform", "Vectorscope", "Level Indicator"}
    printf("Overlays:\n")
    sleep(2)
    local available_overlays = {}
    local old_state = {}
    for i,feature in pairs(all_overlays) do
        old_state[feature] = menu.get("Overlay", feature)
        if old_state[feature] ~= nil then
            table.insert(available_overlays, feature)
            printf("- %s: %s\n", feature, old_state[feature]);
            sleep(1)
        end
    end
    printf("Turning everything off:\n")
    sleep(2)
    for i,feature in pairs(available_overlays) do
        if old_state[feature] ~= "OFF" then
            printf("- %s: %s -> OFF\n", feature, old_state[feature]);
            assert(menu.set("Overlay", feature, "OFF"))
            assert(menu.get("Overlay", feature) == "OFF")
            sleep(1)
        end
    end
    printf("Turning on one by one:\n")
    sleep(2)
    for i,feature in pairs(available_overlays) do
        printf("- %s: ON/OFF", feature);
        assert(menu.set("Overlay", feature, "ON"))
        assert(menu.get("Overlay", feature) ~= "OFF")
        printf(" (%s)\n", menu.get("Overlay", feature));
        sleep(2)
        assert(menu.set("Overlay", feature, "OFF"))
        assert(menu.get("Overlay", feature) == "OFF")
        sleep(1)
    end
    printf("Turning everything on:\n")
    sleep(2)
    for i,feature in pairs(available_overlays) do
        printf("- %s: ON\n", feature);
        assert(menu.set("Overlay", feature, "ON"))
        assert(menu.get("Overlay", feature) ~= "OFF")
        sleep(1)
    end
    sleep(5)
    printf("Restoring previous state:\n")
    sleep(2)
    for i,feature in pairs(available_overlays) do
        printf("- %s: %s\n", feature, old_state[feature]);
        assert(menu.set("Overlay", feature, old_state[feature]))
        assert(menu.get("Overlay", feature) == old_state[feature])
    end
    printf("Overlays working :)\n")
    sleep(2)
end
function test_lv()
    request_mode(MODE.M, "M")
    printf("Testing module 'lv'...\n")
    if lv.enabled then
        printf("LiveView is running; stopping...\n")
        lv.stop()
        assert(not lv.enabled, "LiveView did not stop")
        sleep(2)
    end
    assert(not lv.enabled)
    assert(lv.vidmode == "PH-NOLV")
    printf("Starting LiveView...\n")
    lv.start()
    assert(lv.enabled, "LiveView did not start")
    assert(not lens.autofocusing)
    assert(lv.vidmode == "PH-LV")
    assert(camera.gui.idle == true)
    sleep(2)
    local function print_overlays_status()
        printf("Overlays: %s\n", lv.overlays == 1 and "Canon" or lv.overlays == 2 and "ML" or "disabled");
    end
    console.hide(); assert(not console.visible)
    local old_gdr = menu.get("Overlay", "Global Draw")
    local overlays_tested = false
    for i=1,16 do
        key.press(KEY.INFO)
        sleep(0.2); print_overlays_status()
        sleep(1)
        if lv.enabled and lv.overlays ~= 1 then
            assert(menu.set("Overlay", "Global Draw", "ON"))
            sleep(0.2); print_overlays_status()
            assert(lv.overlays == 2)
            sleep(1)
            if not overlays_tested then
                test_ml_overlays()
                overlays_tested = true
                sleep(1)
            end
            assert(menu.set("Overlay", "Global Draw", "OFF"))
            sleep(0.2); print_overlays_status()
            assert(lv.overlays == false)
            sleep(1)
            if i > 8 then break end
        end
    end
    assert(menu.set("Overlay", "Global Draw", old_gdr))
    assert(menu.get("Overlay", "Global Draw") == old_gdr)
    sleep(0.2); print_overlays_status()
    console.show(); assert(console.visible)
    sleep(2)
    local old_x5 = menu.get("LiveView zoom tweaks", "Zoom x5")
    local old_x10 = menu.get("LiveView zoom tweaks", "Zoom x10")
    assert(menu.set("LiveView zoom tweaks", "Zoom x5", "ON"))
    assert(menu.set("LiveView zoom tweaks", "Zoom x10", "ON"))
    for i,z in pairs{1, 5, 10, 5, 1, 10, 1} do
        printf("Setting zoom to x%d...\n", z)
        lv.zoom = z
        assert(lv.zoom == z, "Could not set zoom in LiveView ")
        if z == 5 then
            assert(lv.vidmode == "ZOOM-X5");
        elseif z == 10 then
            assert(lv.vidmode == "ZOOM-X10");
        else
            assert(lv.vidmode == "PH-LV")
        end
        lv.wait(5)
    end
    assert(menu.set("LiveView zoom tweaks", "Zoom x5", old_x5))
    assert(menu.set("LiveView zoom tweaks", "Zoom x10", old_x10))
    assert(menu.get("LiveView zoom tweaks", "Zoom x5") == old_x5)
    assert(menu.get("LiveView zoom tweaks", "Zoom x10") == old_x10)
    printf("Pausing LiveView...\n")
    lv.pause()
    assert(lv.enabled, "LiveView stopped")
    assert(lv.paused, "LiveView could not be paused")
    assert(not lv.running, "LiveView should not be running")
    assert(not lens.autofocusing)
    assert(lv.vidmode == "PAUSED-LV");
    sleep(2)
    printf("Resuming LiveView...\n")
    lv.resume()
    assert(lv.enabled, "LiveView stopped")
    assert(not lv.paused, "LiveView could not be resumed")
    assert(not lens.autofocusing)
    assert(lv.vidmode == "PH-LV");
    sleep(2)
    printf("Stopping LiveView...\n")
    lv.stop()
    assert(not lv.enabled, "LiveView did not stop")
    assert(not lv.paused,  "LiveView is disabled, can't be paused")
    assert(not lv.running, "LiveView is disabled, can't be running")
    assert(not lens.autofocusing)
    assert(lv.vidmode == "PH-NOLV")
    sleep(1)
    printf("LiveView tests completed.\n")
    printf("\n")
end
function test_lens_focus()
    printf("\n")
    printf("Testing lens focus functionality...\n")
    if lens.name == "" then
        printf("This test requires an electronic lens.\n")
        assert(not lens.af, "manual lenses can't autofocus")
        return
    end
    if not lens.af then
        printf("Please enable autofocus.\n")
        printf("(or, remove the lens from the camera to skip this test)\n")
        while not lens.af and lens.name ~= "" do
            console.show(); assert(console.visible)
            sleep(1)
            alert()
        end
        sleep(1)
    end
    local function try_autofocus()
        if lens.autofocus() then
            return
        end
        printf("Is there something to focus on?\n")
        for i = 1,30 do
            alert()
            printf("\b\b\b\b\b%d...", 30 - i)
            io.flush()
            sleep(1)
            if lens.autofocus() then return end
        end
        assert(false, "could not autofocus")
    end
    if not lv.running then
        if lens.af then
            printf("Autofocus outside LiveView...\n")
            try_autofocus()
        end
        lv.start()
        assert(lv.running)
    end
    if lens.af then
        printf("Focus distance: %s\n",  lens.focus_distance)
        printf("Autofocus in LiveView...\n")
        assert(not lens.autofocusing)
        try_autofocus()
        assert(not lens.autofocusing)
        printf("Please trigger autofocus (half-shutter / AF-ON / * ).\n")
        for i = 1,6000 do
            msleep(10)
            if lens.autofocusing then
                printf("Autofocus triggered.\n")
                while lens.autofocusing do
                    msleep(10)
                end
                printf("Autofocus completed.\n")
                break
            end
            if i % 100 == 0 then
                printf("\b\b\b\b\b%d...", 60 - i // 100)
                io.flush()
                alert()
            end
            if i // 100 == 60 then
                printf("\b\b\b\b\b")
                io.flush()
                assert(false, "Autofocus not triggered.\n")
            end
        end
        sleep(1)
        printf("Focus distance: %s\n",  lens.focus_distance)
        sleep(5)
        printf("Focusing backward...\n")
        while lens.focus(-1,3,true) do end
        sleep(0.5)
        printf("Focus distance: %s\n",  lens.focus_distance)
        printf("Focus motor position: %d\n", lens.focus_pos)
        for i,step in pairs{3,2} do
            for j,wait in pairs{true,false} do
                printf("Focusing forward with step size %d, wait=%s...\n", step, wait)
                local steps_front = 0
                local focus_pos_0 = lens.focus_pos
                while lens.focus(1,step,wait) do
                    printf(".")
                    steps_front = steps_front + 1
                end
                sleep(0.5)
                printf("\n")
                printf("Focus distance: %s\n",  lens.focus_distance)
                printf("Focus motor position: %d\n", lens.focus_pos)
                local focus_pos_1 = lens.focus_pos
                sleep(0.5)
                printf("Focusing backward with step size %d, wait=%s...\n", step, wait)
                local steps_back = 0
                while lens.focus(-1,step,wait) do
                    printf(".")
                    steps_back = steps_back + 1
                end
                sleep(0.5)
                printf("\n")
                printf("Focus distance: %s\n",  lens.focus_distance)
                printf("Focus motor position: %d\n", lens.focus_pos)
                local focus_pos_2 = lens.focus_pos
                sleep(0.5)
                local motor_steps_front = math.abs(focus_pos_1 - focus_pos_0)
                local motor_steps_back  = math.abs(focus_pos_2 - focus_pos_1)
                local motor_steps_lost  = math.abs(focus_pos_2 - focus_pos_0)
                printf("Focus range: %s steps forward, %s steps backward. \n",  steps_front, steps_back)
                printf("Motor steps: %s forward, %s backward, %s lost. \n",  motor_steps_front, motor_steps_back, motor_steps_lost)
                sleep(0.5)
            end
        end
        printf("\nFocus test completed.\n")
    else
        printf("Focus test skipped.\n")
    end
    printf("\n")
end
function test_movie()
    printf("\n")
    printf("Testing movie recording...\n")
    assert(camera.mode ~= MODE.MOVIE)
    local s,e = pcall(movie.start)
    assert(s == false)
    assert(e:find("movie mode"))
    request_mode(MODE.MOVIE, "Movie")
    lv.start()
    assert(lv.running)
    lv.pause()
    local s,e = pcall(movie.start)
    assert(s == false)
    assert(e:find("LiveView") or e:find("movie mode"))
    menu.close()
    lv.resume()
    menu.open()
    local s,e = pcall(movie.start)
    assert(s == false)
    assert(e:find("menu"))
    menu.close()
    console.hide(); assert(not console.visible)
    movie.start()
    assert(movie.recording)
    sleep(1)
    movie.stop()
    assert(not movie.recording)
    console.show(); assert(console.visible)
    assert(camera.gui.idle == true)
    camera.gui.play = true
    assert(camera.gui.play == true)
    if camera.model ~= "50D" then
        assert(camera.gui.play_movie == true)
        assert(camera.gui.play_photo == false)
    end
    printf("Movie recording tests completed.\n")
    printf("\n")
end
function api_tests()
    menu.close()
    console.clear()
    console.show()
    test_log = logger("ML/LOGS/LUATEST.LOG")
    local s,e = xpcall(function()
        strict_tests()
        generic_tests()
        printf("Module tests...\n")
        test_io()
        test_camera_gui()
        test_menu()
        test_camera_take_pics()
        sleep(1)
        test_multitasking()
        test_keys()
        test_lv()
        test_lens_focus()
        test_camera_exposure()
        test_movie()
        printf("Done!\n")
    end, debug.traceback)
    if s == false then
        test_log:write("\n")
        test_log:write(e)
        test_log:close()
    else
        test_log:close()
        key.wait()
        console.hide()
    end
end
assert(#arg == 0)
assert(arg[0] == "API_TEST.LUA" or arg[0] == "api_test.lua")
api_tests()
