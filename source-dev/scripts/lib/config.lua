config = {}
config.configs = {}
config.__index = config
local create_internal = function(default,thisfile)
    local cfg = {}
    assert(thisfile ~= nil, "Could not determine script filename")
    local short_name = string.match(thisfile,"/([^/%.]+)%.[^/%.]+$")
    cfg.filename = string.format("%s%s.cfg", dryos.config_dir.path,short_name)
    assert(thisfile ~= cfg.filename, "Could not determine config filename")
    cfg.default = default
    setmetatable(cfg,config)
    cfg.data = cfg:load()
    table.insert(config.configs, cfg)
    if event.config_save == nil then
        event.config_save = function(unused)
            for i,v in ipairs(config.configs) do
                v:saving()
                v:save()
            end
        end
    end
    return cfg
end
function config.create(default)
    return create_internal(default, debug.getinfo(2,"S").short_src)
end
function config.create_from_menu(m)
    local default = {}
    default[m.name] = m.value
    if m.submenu ~= nil then
        for k,v in pairs(m.submenu) do
            default[k] = v.value
        end
    end
    local cfg = create_internal(default,debug.getinfo(2,"S").short_src)
    cfg.menu = m
    m.value = cfg.data[m.name]
    if m.submenu ~= nil then
        for k,v in pairs(m.submenu) do
             v.value = cfg.data[k]
        end
    end
    return cfg
end
function config:load()
    local status,result = pcall(dofile,self.filename)
    if status and result ~= nil then
        return result
    else
        print(result)
        return self.default
    end
end
function config:saving()
    if self.menu ~= nil then
        self.data[self.menu.name] = self.menu.value
        if self.menu.submenu ~= nil then
            for k,v in pairs(self.menu.submenu) do
                self.data[k] = v.value
            end
        end
    end
end
function config:save()
    local f = io.open(self.filename,"w")
    f:write("return ")
    assert(f ~= nil, "Could not save config: "..self.filename)
    config.serialize(f,self.data)
    f:close()
end
function config.serialize(f,o)
    if type(o) == "number" then
        f:write(tostring(o))
    elseif type(o) == "string" then
        f:write(string.format("%q", o))
    elseif type(o) == "table" then
        f:write("{\n")
        for k,v in pairs(o) do
            f:write("  [")
            config.serialize(f,k)
            f:write("] = ")
            config.serialize(f,v)
            f:write(",\n")
        end
        f:write("}\n")
    else
    end
end
return config
