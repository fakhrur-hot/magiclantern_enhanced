-- Copy to M
-- Copy exposure settings to M mode
require("config")
prop_value = {}
prop_value.__index = prop_value
function prop_value.create(...)
    local pv =
    {
        value = 0,
        previous = 0,
        time = dryos.ms_clock,
        props = {...}
    }
    setmetatable(pv,prop_value)
    return pv
end
function prop_value:enable()
    for i,v in ipairs(self.props) do
        function v.handler(prop,value) self:set(value) end
    end
end
function prop_value:disable()
    for i,v in ipairs(self.props) do
        v.handler = nil
    end
end
function prop_value:set(value)
    if camera.mode ~= MODE.M and value ~= 0 then
        self.previous = self.value
        self.value = value
    end
end
function prop_value:get()
    if dryos.ms_clock - self.time < 1000 then
        return self.previous
    else
        return self.value
    end
end
local shutter_value = prop_value.create(property.SHUTTER,property.SHUTTER_AUTO)
local aperture_value = prop_value.create(property.APERTURE,property.APERTURE_AUTO)
local iso_value = prop_value.create(property.ISO,property.ISO_AUTO)
function shooting_mode_handler(self,value)
    if value == MODE.M then
        local s = shutter_value:get()
        if s ~= 0 then camera.shutter.raw = s end
        local a = aperture_value:get()
        if a ~= 0 then camera.aperture.raw = a end
        local i = iso_value:get()
        if i ~= 0 then camera.iso.raw = i end
    else
        shutter_value.time = dryos.ms_clock
        aperture_value.time = dryos.ms_clock
        iso_value.time = dryos.ms_clock
        if value == MODE.AV or value == MODE.TV or value == MODE.P then
            camera.iso.raw = 0
        end
    end
end
copy2m_menu = menu.new
{
    parent  = "Copy exposure to M",
    name    = "Enabled",
    help    = "Copy exposure settings (Tv, Av, ISO) when switching to M.",
    choices = {"OFF","ON"},
    value   = "OFF"
}
function copy2m_menu:select(delta)
    if self.value == "OFF" then self.value = "ON" else self.value = "OFF" end
    copy2m_update(self.value)
end
function copy2m_update(value)
    if value == "ON" then
        shutter_value:enable()
        aperture_value:enable()
        iso_value:enable()
        property.SHOOTING_MODE.handler = shooting_mode_handler
    else
        shutter_value:disable()
        aperture_value:disable()
        iso_value:disable()
        property.SHOOTING_MODE.handler = nil
    end
end
config.create_from_menu(copy2m_menu)
copy2m_update(copy2m_menu.value)
