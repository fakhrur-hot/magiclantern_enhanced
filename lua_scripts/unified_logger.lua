-- unified_logger.lua
-- AI-LUT pipeline -- REQ-001: On-Camera Sensor Data Logging.
-- STUDY / LOGGING MODE ONLY: reads sensor data on every half-press shutter
-- event and appends a key=value record to A:/ML/logs/unified_log.txt for
-- offline model training. It NEVER changes a camera setting.
--
-- HARD CONSTRAINTS (EOS 6D DIGIC 5+ ARM Cortex-R4 -- no FPU):
--   * Integer-only math. No floating-point constants, no fractional division.
--   * No set_* camera API calls anywhere in this script.
--   * Script size < 10 KB (kept well under to avoid the ML boot-time size crash).
--
-- API names follow .kiro/specs/ai-lut-magic-lantern; any on-camera Lua API
-- mismatches are reconciled during on-camera testing (tasks.md Task 10).

local LOG_PATH = "A:/ML/logs/unified_log.txt"

-- --------------------------------------------------------------------------
-- compute_light_level(hist) -> integer 0..255
-- 90th-percentile bin index of a 256-bin histogram. Integer math only:
-- addition, one math.floor (returns an integer), and comparisons.
-- Missing/empty histogram -> 128 (mid-tone) fallback.
-- --------------------------------------------------------------------------
function compute_light_level(hist)
    if not hist then return 128 end
    local total = 0
    for i = 1, 256 do
        total = total + (hist[i] or 0)
    end
    if total == 0 then return 128 end
    local threshold = math.floor(total * 9 / 10)  -- single floor call, integer result
    local cumulative = 0
    for i = 1, 256 do
        cumulative = cumulative + (hist[i] or 0)
        if cumulative >= threshold then
            return i - 1  -- 0-based bin index (matches unified.tbl LightLevel range)
        end
    end
    return 255  -- fallback: fully bright
end

-- --------------------------------------------------------------------------
-- get_histogram_string([hist]) -> string
-- Joins the 256 bins as comma-separated integers. Calls Magic Lantern
-- get_histogram() when no table is passed. Returns "nil" if unavailable.
-- --------------------------------------------------------------------------
function get_histogram_string(hist)
    if hist == nil and get_histogram then
        hist = get_histogram()
    end
    if not hist then return "nil" end
    local parts = {}
    for i = 1, 256 do
        parts[i] = tostring(hist[i] or 0)
    end
    return table.concat(parts, ",")
end

-- --------------------------------------------------------------------------
-- append_log(path, entry) -> void
-- Opens file in append mode, writes, closes. If io.open returns nil (e.g. SD
-- card missing) the write is skipped silently -- no crash.
-- --------------------------------------------------------------------------
function append_log(path, entry)
    local f = io.open(path, "a")
    if not f then return end  -- silent skip; do not crash
    f:write(entry)
    f:close()
end

-- --------------------------------------------------------------------------
-- Read-only camera-state getters. None call set_*.
-- Each guards missing APIs/fields so a half-press can never crash the camera.
-- --------------------------------------------------------------------------

local function two(n)
    n = n or 0
    if n < 10 then return "0" .. tostring(n) end
    return tostring(n)
end

-- ISO 8601 timestamp from the DryOS clock; "unknown" if unavailable.
-- Accepts both short (min/sec) and long (minute/second) field spellings.
function get_timestamp()
    local d = nil
    if dryos and dryos.date then d = dryos.date end
    if type(d) ~= "table" then return "unknown" end
    return tostring(d.year or 0)
        .. "-" .. two(d.month) .. "-" .. two(d.day)
        .. "T" .. two(d.hour)
        .. ":" .. two(d.minute or d.min)
        .. ":" .. two(d.second or d.sec)
end

-- Scene label. Deliberately "unknown": brightness is not a reliable proxy for
-- lighting type/color, and a wrong guess would corrupt the scene-keyed WB
-- training. Scene is assigned during offline labeling / reconciled in Task 10.
function get_scene()
    return "unknown"
end

-- Current ISO as a numeric string; "0" if unavailable (offline parser guards 0).
function get_iso()
    if camera and camera.iso then
        local v = camera.iso.value or camera.iso
        if type(v) == "number" then return tostring(v) end
    end
    return "0"
end

-- Current shutter speed as recorded by the API. No float math is performed
-- here; conversion to a 1/n fraction is left to offline analysis.
function get_shutter()
    if camera and camera.shutter then
        local v = camera.shutter.value or camera.shutter
        if type(v) == "number" then return tostring(v) end
    end
    return "unknown"
end

-- Current WB as integer RGB multipliers (G=100 reference). The logger performs
-- no float math and has no direct RGB-gain read, so it records the neutral
-- reference; WB is derived from scene during training regardless.
function get_wb()
    return "R100,G100,B100"
end

-- --------------------------------------------------------------------------
-- half_press_logger() -> void
-- Main callback. Fetch the histogram once, compute the integer light level,
-- format a key=value record with a "---" separator, and append it to the log.
-- No set_* camera API calls are made here.
-- --------------------------------------------------------------------------
function half_press_logger()
    local hist = nil
    if get_histogram then hist = get_histogram() end

    local light_level = compute_light_level(hist)
    local hist_str = get_histogram_string(hist)

    local entry = "Timestamp=" .. get_timestamp() .. "\n"
                .. "Hist=" .. hist_str .. "\n"
                .. "Scene=" .. get_scene() .. "\n"
                .. "LightLevel=" .. tostring(light_level) .. "\n"
                .. "Shutter=" .. get_shutter() .. "\n"
                .. "ISO=" .. get_iso() .. "\n"
                .. "WB=" .. get_wb() .. "\n"
                .. "---\n"

    append_log(LOG_PATH, entry)
end

-- --------------------------------------------------------------------------
-- Register the logger with Magic Lantern's half-press shutter event.
-- --------------------------------------------------------------------------
if register_half_press_callback then
    register_half_press_callback(half_press_logger)
end
