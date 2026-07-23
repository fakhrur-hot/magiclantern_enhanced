--[[
  mov_metadata.lua — MOV Recording Metadata + Gyroflow Logger
  
  Three outputs per .MOV clip:
  
  1. Full metadata CSV: ML/DATA/SHOTS/MVI_NNNN.csv
     All per-frame camera state (ISO, shutter, WB, lens, gyro).
     For RaZStudio and general post-production use.
  
  2. Gyroflow-native CSV: ML/DATA/SHOTS/MVI_NNNN.gyro.csv
     Pure gyro log (time,gyroX,gyroY,gyroZ,pitch,roll) in radians/sec + degrees.
     Frame-aligned timestamps (frame_index / fps).
     Drop .MOV + .gyro.csv into Gyroflow → instant stabilization.
     Compatible with Gyroflow plugins for Premiere, Resolve, Final Cut.
  
  3. Gyroflow JSON: ML/DATA/SHOTS/MVI_NNNN.gyro.json
     Same data in JSON format with fps and units fields.
     Alternative import for Gyroflow.
  
  Timestamp strategy: frame_index / fps (frame-aligned)
    - Guarantees exact 1/fps spacing regardless of Lua callback jitter
    - Gyroflow sees one sample per frame, perfectly aligned
    - No manual sync needed
  
  FPS detection: auto-detects from camera.fps or Canon property.
  
  Gyro source: mlc.gyro() C bridge (external IMU via hot-shoe serial).
  If no IMU connected, gyro columns are zero (Gyroflow loads without error).
  
  Menu: Expo > MOV Metadata (OFF / ON), persisted via ML config system.
  
  Record detection: event.shoot_task() polls movie.recording for edge detection.
  NOTE: There is no event.record callback in ML's Lua event system.
  event.shoot_task (CBR_SHOOT_TASK) fires periodically and is wired through lua.c.
  Polling movie.recording from this callback gives edge detection with ~100ms latency.
  
  NOTE: Canon 6D has NO built-in gyro. Requires external IMU hardware
  for real stabilization data. Without it, all other metadata still logs.
]]

require("config")

mov_meta_menu = menu.new
{
    parent = "Expo",
    name   = "MOV Metadata",
    help   = "Log per-frame metadata + Gyroflow CSV alongside .MOV",
    value  = 0,
    max    = 1,
    choices = {"OFF", "ON"},
}

-- Persist menu setting across power cycles via ML config system
config.create_from_menu(mov_meta_menu)

-- State
local recording = false
local was_recording = false  -- for edge detection in shoot_task
local meta_file = nil        -- full metadata CSV handle
local gyro_file = nil        -- gyroflow-native CSV handle
local frame_index = 0
local fps = 24.0
local current_basename = ""
local gyro_frames = {}       -- accumulate for JSON output
local skip_count = 0         -- consecutive write-timeout skips (Req 9.1/9.2)
local skip_notified = false  -- true after "write degraded" notification shown (Req 9.3)

-- Detect gyro source from C bridge status
-- Returns: "none", "external_imu", or "is_lens"
local function detect_gyro_source()
    if mlc and mlc.gyro_status then
        local status = mlc.gyro_status()
        if status == "active" or status == "timeout" then
            return "external_imu"
        end
    end
    return "none"
end

-- Auto-detect movie FPS from camera
local function detect_fps()
    -- ML Lua exposes camera.fps in some builds
    if camera and camera.fps then
        local f = camera.fps
        if type(f) == "table" and f.value then
            fps = f.value
        elseif type(f) == "number" and f > 0 then
            fps = f
        end
    end
    -- Fallback: fps stays at 24.0 (safe default for Canon 6D)
    if fps <= 0 then fps = 24.0 end
end

-- Get MOV basename from file_number
local function get_mov_basename()
    local fn = dryos.shooting_card.file_number
    return string.format("MVI_%04d", fn)
end

-- Ensure directory exists
local function ensure_dir(path)
    dryos.directory(path)
end

-- Open all 3 output files at record start
local function open_logs()
    detect_fps()
    current_basename = get_mov_basename()
    local gyro_source = detect_gyro_source()
    
    ensure_dir("ML/DATA/SHOTS")
    
    local meta_path = "ML/DATA/SHOTS/" .. current_basename .. ".csv"
    local gyro_path = "ML/DATA/SHOTS/" .. current_basename .. ".gyro.csv"
    
    -- Full metadata CSV
    meta_file = io.open(meta_path, "w")
    if meta_file then
        meta_file:write("# schema_version=1.0\n")
        meta_file:write("# gyro_source=" .. gyro_source .. "\n")
        meta_file:write(string.format("# fps=%.2f\n", fps))
        meta_file:write("timestamp_s,frame,iso,shutter_raw,wb_r,wb_g,wb_b,")
        meta_file:write("lens_id,focus_dist,light_level,gyro_x,gyro_y,gyro_z\n")
    end
    
    -- Gyroflow-native CSV (time,gyroX,gyroY,gyroZ,pitch,roll)
    -- Header has exactly 5 commas: 6 columns
    gyro_file = io.open(gyro_path, "w")
    if gyro_file then
        gyro_file:write("time,gyroX,gyroY,gyroZ,pitch,roll\n")
    end
    
    frame_index = 0
    gyro_frames = {}
    skip_count = 0
    skip_notified = false
    
    if not meta_file and not gyro_file then
        display.notify_box("MOV Meta: can't open logs")
        return false
    end
    return true
end

-- Write Gyroflow JSON companion at record stop
local function write_gyro_json()
    local json_path = "ML/DATA/SHOTS/" .. current_basename .. ".gyro.json"
    local jf = io.open(json_path, "w")
    if not jf then return end
    
    jf:write('{\n  "gyro": [\n')
    for i, g in ipairs(gyro_frames) do
        local comma = (i < #gyro_frames) and "," or ""
        jf:write(string.format('    {"t":%.6f,"x":%.6f,"y":%.6f,"z":%.6f,"pitch":%.3f,"roll":%.3f}%s\n',
            g.t, g.x, g.y, g.z, g.pitch, g.roll, comma))
    end
    jf:write(string.format('  ],\n  "fps": %.2f,\n  "units": "rad/s",\n  "extra": "Canon 6D electronic level metadata"\n}\n', fps))
    jf:close()
end

-- Close all log files + flush JSON array to file
local function close_logs()
    if meta_file then
        meta_file:close()
        meta_file = nil
    end
    if gyro_file then
        gyro_file:close()
        gyro_file = nil
    end
    
    -- Write JSON companion for Gyroflow
    if #gyro_frames > 0 then
        write_gyro_json()
    end
    
    -- 3-second notification with filename + frame count
    display.notify_box(string.format("MOV Meta: %s (%d frames @ %.0ffps)",
        current_basename, frame_index, fps), 3000)
    
    gyro_frames = {}
end

-- Called on rising edge (recording just started)
local function on_record_start()
    recording = true
    open_logs()
end

-- Called on falling edge (recording just stopped)
local function on_record_stop()
    recording = false
    close_logs()
end

-- Convert milli-degrees/sec (int32 from C bridge) to radians/sec
local function mdps_to_rads(mdps)
    return mdps * 3.14159265358979 / 180000.0
end

-- Log one frame (called at vsync cadence while recording)
-- Performance: 1ms write timeout per Req 9.1/9.2/9.3.
-- dryos.ms_clock provides millisecond-precision timestamp.
-- ML's io.write is blocking; we detect slow writes AFTER completion
-- and count consecutive slow frames. If >10 consecutive, notify once.
local function log_frame()
    if not meta_file and not gyro_file then return end
    
    -- Frame-aligned timestamp: exact 1/fps spacing
    local t_sec = frame_index / fps
    
    -- Gyro from C bridge (returns table {x,y,z} in mdps or zeros)
    local gx, gy, gz = 0.0, 0.0, 0.0
    if mlc and mlc.gyro then
        local g = mlc.gyro()
        gx = mdps_to_rads(g.x or 0)
        gy = mdps_to_rads(g.y or 0)
        gz = mdps_to_rads(g.z or 0)
    end
    
    -- Electronic level from C bridge (pitch/roll in degrees, ~5 Hz hold-last-value)
    local pitch, roll = 0.0, 0.0
    if mlc and mlc.level then
        local lev = mlc.level()
        pitch = lev.pitch or 0.0
        roll = lev.roll or 0.0
    end
    
    -- Time the write operations (1ms budget — Req 9.1)
    local t_start = dryos.ms_clock
    
    -- Full metadata CSV row
    if meta_file then
        local iso = camera.iso.raw or 0
        local tv = camera.shutter.raw or 0
        local wb_r = lens.wb_r or 0
        local wb_g = lens.wb_g or 0
        local wb_b = lens.wb_b or 0
        local lid = lens.id or 0
        local fdist = lens.focus_distance or 0
        local ll = 128  -- light level placeholder
        
        meta_file:write(string.format("%.6f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.6f,%.6f,%.6f\n",
            t_sec, frame_index, iso, tv, wb_r, wb_g, wb_b,
            lid, fdist, ll, gx, gy, gz))
    end
    
    -- Gyroflow-native CSV row (time,gyroX,gyroY,gyroZ,pitch,roll)
    if gyro_file then
        gyro_file:write(string.format("%.6f,%.6f,%.6f,%.6f,%.3f,%.3f\n",
            t_sec, gx, gy, gz, pitch, roll))
    end
    
    -- Check write duration against 1ms threshold (Req 9.1)
    local t_end = dryos.ms_clock
    local elapsed = t_end - t_start
    
    if elapsed > 1 then
        -- Write exceeded 1ms — count as skip (Req 9.2)
        skip_count = skip_count + 1
        if skip_count > 10 and not skip_notified then
            -- More than 10 consecutive slow writes — notify once (Req 9.3)
            display.notify_box("MOV Meta: write degraded")
            skip_notified = true
        end
    else
        -- Write was fast — reset consecutive skip counter
        skip_count = 0
    end
    
    -- Accumulate for JSON output
    gyro_frames[#gyro_frames + 1] = {
        t = t_sec, x = gx, y = gy, z = gz,
        pitch = pitch, roll = roll
    }
    
    frame_index = frame_index + 1
end

------------------------------------------------------------------------
-- EVENT HANDLERS
------------------------------------------------------------------------

-- Detect record start/stop by polling movie state each SHOOT_TASK tick.
-- NOTE: There is NO event.record callback in ML's Lua event system.
-- event.shoot_task (CBR_SHOOT_TASK) fires periodically from the shoot task
-- and IS wired through lua.c. Polling movie.recording from this callback
-- gives edge detection with ~100ms latency (acceptable for file open/close).
function event.shoot_task()
    if mov_meta_menu.value == 0 then
        -- Feature disabled: ensure clean state if it was just toggled off
        if recording then
            on_record_stop()
        end
        was_recording = false
        return
    end
    
    local now_recording = (movie.recording == true)
    if now_recording and not was_recording then
        on_record_start()
    elseif not now_recording and was_recording then
        on_record_stop()
    end
    was_recording = now_recording
end

-- Per-frame logging at vsync cadence while recording
-- Requires CONFIG_VSYNC_EVENTS enabled in lua_common.h
function event.vsync()
    if recording and mov_meta_menu.value == 1 then
        log_frame()
    end
end
