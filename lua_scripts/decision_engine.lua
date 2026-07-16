-- decision_engine.lua
-- AI-LUT pipeline -- REQ-003: On-Camera LUT Decision Engine.
-- Each half-press: load unified.tbl, compute integer light level, look up the
-- decision (exact -> nearest-neighbor -> unknown -> fallback), apply the five
-- decisions, and log the result.
--
-- CONSTRAINTS (EOS 6D DIGIC 5+ ARM Cortex-R4, no FPU): integer-only math, no
-- float constants, no fractional division; math.floor ONLY in
-- compute_light_level; script kept < 8 KB (ML boot-time size crash threshold).
-- API names follow .kiro/specs/ai-lut-magic-lantern; on-camera mismatches are
-- reconciled in tasks.md Task 10.

local MODEL_PATH = "A:/ML/models/unified.tbl"
local LOG_PATH   = "A:/ML/logs/unified_log.txt"
local WB_NEUTRAL = "R100,G100,B100"

-- Safe defaults when no LUT row matches at all.
local FALLBACK = {
    ETTR = "keep_shutter", ALO = "neutral", HTP = "priority_off",
    ISO = "400", WB = "R100,G100,B100",
}

-- Crash-safe append: skip silently if the file cannot be opened.
local function append_log(path, entry)
    local f = io.open(path, "a")
    if not f then return end
    f:write(entry)
    f:close()
end

local function two(n)
    n = n or 0
    if n < 10 then return "0" .. tostring(n) end
    return tostring(n)
end

-- ISO 8601 timestamp from the DryOS clock; "unknown" if unavailable.
local function get_timestamp()
    local d = nil
    if dryos and dryos.date then d = dryos.date end
    if type(d) ~= "table" then return "unknown" end
    return tostring(d.year or 0) .. "-" .. two(d.month) .. "-" .. two(d.day)
        .. "T" .. two(d.hour) .. ":" .. two(d.minute or d.min)
        .. ":" .. two(d.second or d.sec)
end

-- compute_light_level(hist) -> integer 0..255. Identical to the logger
-- (duplicated so each script is self-contained). Integer-only; single floor.
function compute_light_level(hist)
    if not hist then return 128 end
    local total = 0
    for i = 1, 256 do total = total + (hist[i] or 0) end
    if total == 0 then return 128 end
    local threshold = math.floor(total * 9 / 10)  -- the ONLY math.floor
    local cumulative = 0
    for i = 1, 256 do
        cumulative = cumulative + (hist[i] or 0)
        if cumulative >= threshold then return i - 1 end
    end
    return 255
end

-- load_unified_model(path) -> table keyed by "<scene>_<light>". Empty table
-- (and LUT_NOT_FOUND logged) if the file is missing/unreadable.
function load_unified_model(path)
    local model = {}
    local f = io.open(path, "r")
    if not f then
        append_log(LOG_PATH, "LUT_NOT_FOUND|path=" .. tostring(path) .. "\n")
        return model
    end
    for line in f:lines() do
        if string.sub(line, 1, 1) ~= "#" and string.find(line, "|") then
            local scene, light, ettr, alo, htp, iso, wb =
                string.match(line,
                    "([^|]+)|(%d+)|([^|]+)|([^|]+)|([^|]+)|(%d+)|([^|]+)")
            if scene and light then
                model[scene .. "_" .. light] = {
                    ETTR = ettr, ALO = alo, HTP = htp, ISO = iso, WB = wb,
                    LightLevel = tonumber(light),
                }
            end
        end
    end
    f:close()
    return model
end

-- find_decision(model, scene, light) -> row or nil. Exact -> scene
-- nearest-neighbor -> "unknown" nearest-neighbor. Integer subtraction only.
function find_decision(model, scene, light_level)
    local key = scene .. "_" .. tostring(light_level)
    if model[key] then return model[key] end

    local prefix = scene .. "_"
    local plen = string.len(prefix)
    local best, best_dist = nil, 999999
    for k, row in pairs(model) do
        if string.sub(k, 1, plen) == prefix then
            local diff = row.LightLevel - light_level
            if diff < 0 then diff = -diff end
            if diff < best_dist then best_dist = diff; best = row end
        end
    end
    if best then return best end

    for k, row in pairs(model) do
        if string.sub(k, 1, 8) == "unknown_" then
            local diff = row.LightLevel - light_level
            if diff < 0 then diff = -diff end
            if diff < best_dist then best_dist = diff; best = row end
        end
    end
    return best  -- nil only if the model is empty
end

-- apply_ettr: keyword -> integer shutter fraction. Pure lookup, no arithmetic.
function apply_ettr(ettr)
    if ettr == "reduce_shutter" then
        set_shutter_fraction(1, 200)
    elseif ettr == "increase_shutter" then
        set_shutter_fraction(1, 50)
    else
        set_shutter_fraction(1, 100)  -- keep_shutter / unknown: neutral
    end
end

-- apply_iso: nil/zero -> fallback ISO 400 + ISO_PARSE_ERROR log.
function apply_iso(iso_str)
    local n = tonumber(iso_str)
    if not n or n == 0 then
        n = 400
        append_log(LOG_PATH, "ISO_PARSE_ERROR|raw=" .. tostring(iso_str)
            .. "|fallback=400\n")
    end
    set_iso(n)
end

-- apply_wb: parse R{n},G{n},B{n} -> set_wb. On failure ALWAYS apply neutral WB
-- (audit fix/REQ-003: never leave WB undefined) and log WB_PARSE_ERROR.
function apply_wb(wb_str)
    local r, g, b = string.match(tostring(wb_str), "R(%d+),G(%d+),B(%d+)")
    if r and g and b then
        set_wb(tonumber(r), tonumber(g), tonumber(b))
    else
        local r0, g0, b0 = string.match(WB_NEUTRAL, "R(%d+),G(%d+),B(%d+)")
        set_wb(tonumber(r0), tonumber(g0), tonumber(b0))
        append_log(LOG_PATH, "WB_PARSE_ERROR|raw=" .. tostring(wb_str)
            .. "|fallback=" .. WB_NEUTRAL .. "\n")
    end
end

-- Phase 2 stubs (Tasks 11/12): present and called every half-press; no-ops.
-- Integer targets: set_alo(2|1|0), set_htp(1|0). No LUT change needed later.
function apply_alo(alo)  -- TODO Phase 2 (Task 11)
end

function apply_htp(htp)  -- TODO Phase 2 (Task 12)
end

-- log_decision: append applied record (same key=value + "---" format as logger).
function log_decision(decision, scene, light)
    append_log(LOG_PATH,
        "Timestamp=" .. get_timestamp() .. "\n"
        .. "Scene=" .. tostring(scene) .. "\n"
        .. "LightLevel=" .. tostring(light) .. "\n"
        .. "Decision_ETTR=" .. tostring(decision.ETTR) .. "\n"
        .. "Decision_ALO=" .. tostring(decision.ALO) .. "\n"
        .. "Decision_HTP=" .. tostring(decision.HTP) .. "\n"
        .. "Decision_ISO=" .. tostring(decision.ISO) .. "\n"
        .. "Decision_WB=" .. tostring(decision.WB) .. "\n"
        .. "---\n")
end

-- half_press_decision() -> void  (main callback)
function half_press_decision()
    local hist = nil
    if get_histogram then hist = get_histogram() end
    if not hist then
        append_log(LOG_PATH, "HIST_NIL|LightLevel=128(fallback)\n")
    end

    local light = compute_light_level(hist)  -- 128 when hist is nil
    local scene = "unknown"                  -- reconciled on-camera (Task 10)

    local model = load_unified_model(MODEL_PATH)
    local decision = find_decision(model, scene, light)
    if not decision then
        decision = FALLBACK
        append_log(LOG_PATH, "FALLBACK_USED|scene=" .. scene
            .. "|light=" .. tostring(light) .. "\n")
    end

    apply_ettr(decision.ETTR)
    apply_iso(decision.ISO)
    apply_wb(decision.WB)
    apply_alo(decision.ALO)
    apply_htp(decision.HTP)

    log_decision(decision, scene, light)
end

if register_half_press_callback then
    register_half_press_callback(half_press_decision)
end
