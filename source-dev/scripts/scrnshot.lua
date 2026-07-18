-- Screenshot
-- Take a screenshot after a delay
scrnshot_menu = menu.new
{
    parent  = "Screenshot on Keypress",
    name    = "Enabled",
    choices = { "OFF", "ON" },
    value   = "OFF",
    help    = "Take a screenshot every time a key is pressed",
}
function event.keypress(key)
    if key ~= 0 and scrnshot_menu.value == "ON" then
        display.screenshot()
    end
end
