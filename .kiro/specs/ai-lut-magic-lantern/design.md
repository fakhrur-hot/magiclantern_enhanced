# Design Document

## Overview

This document describes the technical architecture and component design for the
AI-assisted exposure and color pipeline. The system has four layers: the on-camera
Lua layer (EOS 6D), the offline training layer (Google Colab), the CI/CD layer
(GitHub Actions), and the continuous improvement loop that connects them.

The core constraint: the EOS 6D''s DIGIC 5+ ARM Cortex-R4 cannot run ML inference
on-camera. There is no GPU. The Lua runtime uses integer math only. All
intelligence is computed offline and compressed into a compact `unified.tbl`
stored on the SD card. On-camera Lua does only: file read, table lookup,
integer arithmetic, and Magic Lantern API calls. Nothing else.

---

## Architecture

```
+------------------------------------------------------------------+
|                    CAMERA LAYER (EOS 6D)                        |
|  ARM Cortex-R4  |  Integer Lua only  |  No GPU  |  No FPU      |
|                                                                  |
|  Half-press shutter event                                        |
|         |                                                        |
|         v                                                        |
|  unified_logger.lua --> A:/ML/logs/unified_log.txt              |
|         |                                                        |
|  decision_engine.lua                                            |
|         +-- load_unified_model("A:/ML/models/unified.tbl")      |
|         +-- compute_light_level()   [integer histogram math]    |
|         +-- lookup(scene, light)    [table key, nearest-nbr]    |
|         +-- apply_ettr()   set_shutter_fraction(1, 50/100/200)  |
|         +-- apply_iso()    set_iso(integer)                      |
|         +-- apply_wb()     set_wb(r_int, g_int, b_int)          |
|         +-- apply_alo()    -- TODO: tone curve hook              |
|         +-- apply_htp()    -- TODO: HTP toggle hook              |
|         +-- log_decision() --> A:/ML/logs/unified_log.txt       |
+------------------------------------------------------------------+
                 |
          SD card export (manual)
                 v
+------------------------------------------------------------------+
|            OFFLINE TRAINING LAYER (Google Colab)                |
|  Python 3  |  Pandas  |  scikit-learn  |  No camera dependency  |
|                                                                  |
|  unified_log.txt --> parse_logs() --> DataFrame                  |
|         +-- Feature: LightLevel (int), Scene (str)              |
|         +-- DecisionTreeClassifier(max_depth=4) -> ISO          |
|         +-- Rule-based ETTR / ALO / HTP thresholds (int)        |
|         +-- Scene-based WB multipliers (int, G=100 ref)         |
|         +-- Export --> unified.tbl                              |
+------------------------------------------------------------------+
                 |
          SD card copy + GitHub push
                 v
+------------------------------------------------------------------+
|              CI/CD LAYER (GitHub Actions)                       |
|  build.yml --> matrix (6D, 5D3, 60D, 650D)                     |
|  release.yml --> GitHub Release + per-camera folders            |
+------------------------------------------------------------------+
```

**Data flow:**
```
Camera half-press -> log -> SD card -> Colab train -> unified.tbl
-> SD card copy + git push -> CI build + release
-> decision_engine reads LUT -> applies integer decisions
-> new logs collected -> cycle repeats
```

---

## Components and Interfaces

### Component 1: `unified_logger.lua`

**Platform:** EOS 6D ARM Cortex-R4 Lua. Integer math only. No FPU.

**Purpose:** Study-mode logging only. Captures sensor data during half-press.
Does not alter any camera setting.

**Public interface (Magic Lantern callbacks):**

| Function | Signature | Description |
|---|---|---|
| `half_press_logger` | `() -> void` | Main callback registered with `register_half_press_callback()` |

**Internal functions:**

| Function | Signature | Description |
|---|---|---|
| `append_log` | `(path, entry) -> void` | Opens file in append mode, writes entry, closes |
| `get_histogram_string` | `() -> string` | Calls `get_histogram()`, joins bins as comma-separated integers |
| `compute_light_level` | `(hist_table) -> int` | Returns 90th-percentile bin index (integer 0-255) |

**`compute_light_level` algorithm (integer-only Lua):**
```lua
function compute_light_level(hist)
    -- hist is a table of 256 integer pixel counts
    -- Find total pixel count (integer addition only)
    local total = 0
    for i = 1, 256 do
        total = total + hist[i]
    end
    -- 90th percentile = bin where cumulative count >= 90% of total
    -- Use integer: threshold = total * 9 / 10  (integer division in Lua 5.1)
    local threshold = math.floor(total * 9 / 10)
    local cumulative = 0
    for i = 1, 256 do
        cumulative = cumulative + hist[i]
        if cumulative >= threshold then
            return i - 1  -- 0-based bin index (matches unified.tbl LightLevel range)
        end
    end
    return 255  -- fallback: fully bright
end
```

This is a standard "percentile from histogram" algorithm. It uses only integer
addition, one `math.floor` call, and one comparison per bin. Total iterations:
at most 256. Well within 100 ms budget even on Cortex-R4.

**Log entry format:**
```
Timestamp=2026-07-16T14:07:00
Hist=0,0,12,45,...,3,0,0
Scene=daylight
LightLevel=85
Shutter=1/100
ISO=400
WB=R120,G100,B90
---
```

**Constraints:** No `set_*` API calls. Integer math only. Script <= 10 KB.

---

### Component 2: `decision_engine.lua`

**Platform:** EOS 6D ARM Cortex-R4 Lua. Integer math only. No FPU.

**Purpose:** Full decision engine. Loads LUT on each half-press, applies
decisions to camera, logs results.

**Public interface:**

| Function | Signature | Description |
|---|---|---|
| `half_press_decision` | `() -> void` | Main callback |

**Internal functions:**

| Function | Signature | Description |
|---|---|---|
| `load_unified_model` | `(path) -> table` | Parses `unified.tbl` into Lua table |
| `compute_light_level` | `(hist_table) -> int` | 90th-percentile bin (integer) |
| `find_decision` | `(model, scene, light) -> table` | Exact match then nearest-neighbor |
| `apply_ettr` | `(ettr) -> void` | Maps keyword to `set_shutter_fraction(1, n)` |
| `apply_iso` | `(iso_str) -> void` | Calls `set_iso(tonumber(iso))` |
| `apply_wb` | `(wb_str) -> void` | Parses `R{n},G{n},B{n}`, calls `set_wb(r,g,b)` |
| `apply_alo` | `(alo) -> void` | Phase 1 stub; Phase 2: integer tone curve toggle via ML property hook |
| `apply_htp` | `(htp) -> void` | Phase 1 stub; Phase 2: integer HTP enable/disable via Canon property |
| `log_decision` | `(decision, scene, light) -> void` | Appends result to log file |

**`load_unified_model` logic:**
```lua
function load_unified_model(path)
    local model = {}
    local f = io.open(path, "r")
    if not f then return model end
    for line in f:lines() do
        -- skip comments and lines without pipe separator
        if string.sub(line, 1, 1) ~= "#" and string.find(line, "|") then
            local scene, light, ettr, alo, htp, iso, wb =
                string.match(line, "([^|]+)|(%d+)|([^|]+)|([^|]+)|([^|]+)|(%d+)|([^|]+)")
            if scene and light then
                local key = scene .. "_" .. light
                model[key] = { ETTR=ettr, ALO=alo, HTP=htp, ISO=iso, WB=wb,
                               LightLevel=tonumber(light) }
            end
        end
    end
    f:close()
    return model
end
```

**`find_decision` nearest-neighbor logic (integer math only):**
```lua
function find_decision(model, scene, light_level)
    -- Try exact match first
    local key = scene .. "_" .. tostring(light_level)
    if model[key] then return model[key] end

    -- Nearest-neighbor: scan all rows, find smallest abs difference
    -- for the matching scene. Integer subtraction only.
    -- Performance note: scan is O(n) over LUT rows. Cap LUT at 200 rows
    -- per the REQ-002 <= 1 MB constraint (typical: ~20-40 rows in practice).
    -- Binary search can be added if row count exceeds 100.
    local best = nil
    local best_dist = 999999  -- large integer sentinel
    for k, row in pairs(model) do
        -- Check if this row is for our scene (key starts with scene.."_")
        if string.sub(k, 1, string.len(scene) + 1) == scene .. "_" then
            local diff = row.LightLevel - light_level
            if diff < 0 then diff = -diff end  -- abs via negation (integer)
            if diff < best_dist then
                best_dist = diff
                best = row
            end
        end
    end

    -- If no row for this scene, try "unknown" scene
    if not best then
        for k, row in pairs(model) do
            if string.sub(k, 1, 8) == "unknown_" then
                local diff = row.LightLevel - light_level
                if diff < 0 then diff = -diff end
                if diff < best_dist then
                    best_dist = diff
                    best = row
                end
            end
        end
    end

    return best  -- nil if model is completely empty
end
```

**Performance bound:** With 5 scenes x ~20 light levels = 100 rows, the scan
completes in < 1 ms on Cortex-R4. The 1 MB LUT size limit (REQ-002) naturally
caps row count. If a future LUT exceeds 100 rows per scene, replace the scan
with a pre-sorted array and binary search (bisect-style, integer midpoint).

**ALO / HTP Phase 2 design (integer-only stubs → real hooks):**

ALO (`apply_alo`) will use the Canon property `PROP_AUTO_LIGHOPTIMIZER` or
equivalent Magic Lantern shadow-curve hook. Three integer values will be mapped:

| ALO keyword | Integer action | Canon/ML API target |
|---|---|---|
| `shadow_boost` | Enable ALO, strength=HIGH (int=2) | `set_alo(2)` or property write |
| `shadow_lift` | Enable ALO, strength=LOW (int=1) | `set_alo(1)` |
| `neutral` | Disable ALO (int=0) | `set_alo(0)` |

HTP (`apply_htp`) will use `PROP_HTP` or equivalent Magic Lantern property:

| HTP keyword | Integer action | Canon/ML API target |
|---|---|---|
| `priority_on` | Enable HTP (int=1) | `set_htp(1)` |
| `priority_off` | Disable HTP (int=0) | `set_htp(0)` |

Both hooks remain as `-- TODO Phase 2` stubs in Phase 1. The LUT already
encodes the string keywords; no LUT format change is needed when the hooks
are implemented. Tasks 11 and 12 in `tasks.md` cover this work.



| ETTR keyword | Camera call | Exposure effect |
|---|---|---|
| `reduce_shutter` | `set_shutter_fraction(1, 200)` | 1/200s -- halves exposure, protects highlights |
| `keep_shutter` | `set_shutter_fraction(1, 100)` | 1/100s -- neutral baseline |
| `increase_shutter` | `set_shutter_fraction(1, 50)` | 1/50s -- doubles exposure, lifts shadows |

The denominators 50, 100, 200 form a clean power-of-2 progression (x2 steps).
Selecting between them is a pure string comparison + integer pair lookup.
No arithmetic needed in Lua at apply time.

**WB multiplier apply logic:**
```lua
local WB_NEUTRAL = "R100,G100,B100"  -- safe fallback constant

function apply_wb(wb)
    local r, g, b = string.match(wb, "R(%d+),G(%d+),B(%d+)")
    if r and g and b then
        set_wb(tonumber(r), tonumber(g), tonumber(b))
        -- G is always 100 (reference). R > 100 = warmer. B > 100 = cooler.
        -- Example: R=120, G=100, B=90 = slightly warm (daylight)
        -- Example: R=150, G=100, B=70 = very warm (tungsten)
    else
        -- Parse failed: apply neutral WB so camera is never left in undefined state
        local r0, g0, b0 = string.match(WB_NEUTRAL, "R(%d+),G(%d+),B(%d+)")
        set_wb(tonumber(r0), tonumber(g0), tonumber(b0))
        append_log(LOG_PATH, "WB_PARSE_ERROR|raw=" .. tostring(wb) .. "|fallback=" .. WB_NEUTRAL)
    end
end
```

**Audit fix (WB fallback):** Previously, a WB parse failure would log the error
but skip `set_wb()` entirely, leaving the camera in whatever WB state Canon
firmware last set. The fix always calls `set_wb(100, 100, 100)` on failure,
ensuring a known neutral state. The log entry includes both the raw string that
failed to parse and the fallback value applied, for offline diagnosis.
```

The camera''s `set_wb(r, g, b)` takes integer sensor gain multipliers.
G=100 is the neutral reference. There is no Kelvin temperature on camera --
the LUT stores the final integer values directly.

**Fallback defaults (no LUT match):**
```lua
local FALLBACK = {
    ETTR = "keep_shutter",
    ALO  = "neutral",
    HTP  = "priority_off",
    ISO  = "400",
    WB   = "R100,G100,B100"
}
```

---

### Component 3: `unified.tbl` (LUT file)

**Location:** `A:/ML/models/unified.tbl` on SD card.

**Format:**
```
# Scene|LightLevel|ETTR|ALO|HTP|ISO|WB
daylight|80|reduce_shutter|shadow_lift|priority_off|100|R120,G100,B90
tungsten|20|increase_shutter|shadow_boost|priority_on|1600|R150,G100,B70
shade|60|keep_shutter|neutral|priority_off|400|R110,G100,B105
lowlight|10|increase_shutter|shadow_boost|priority_on|3200|R130,G100,B80
unknown|128|keep_shutter|neutral|priority_off|400|R100,G100,B100
```

**WB multiplier semantics (on-camera meaning):**

| Scene | R | G | B | Rationale |
|-------|---|---|---|-----------|
| daylight | 120 | 100 | 90 | Warm natural light ~5500K; R boost, B reduce |
| shade | 110 | 100 | 105 | Slightly cool ~6500K overcast; small B boost |
| tungsten | 150 | 100 | 70 | Very warm ~3200K incandescent; strong R boost |
| lowlight | 130 | 100 | 80 | Mixed warm sources; moderate adjustments |
| unknown | 100 | 100 | 100 | Neutral -- no correction applied |

These values are camera sensor gains passed directly to `set_wb()`. They are
chosen so that a photograph taken under each lighting condition comes out
with neutral white tones in the CR2 RAW file without further processing.

**ETTR light level thresholds (from REQ-004, integer):**

| Condition | LightLevel range | ETTR decision | Rationale |
|---|---|---|---|
| Near-clipping highlights | > 200 | `reduce_shutter` | 90th percentile bin > 200 signals bright scene; shorten exposure to pull histogram left and protect highlights |
| Normal exposure | 30-200 | `keep_shutter` | Histogram well-distributed; no adjustment |
| Underexposed / dark | < 30 | `increase_shutter` | Very low 90th-percentile = dark scene; lengthen exposure to push histogram right |

**ALO shadow lift thresholds (integer):**

| LightLevel | ALO | Rationale |
|---|---|---|
| < 20 | `shadow_boost` | Very dark scene; aggressive shadow lift to recover detail |
| 20-49 | `shadow_lift` | Moderately dark; gentle shadow lift |
| >= 50 | `neutral` | Adequate brightness; no shadow lift needed |

---

### Component 4: `lut_training.ipynb` / `lut_training_multi_param.py`

**Platform:** Google Colab / Python 3. Runs offline on a host machine.
No camera dependency. No GPU inference at runtime.

**Interface:**
- Input: `unified_log.txt`
- Output: `unified.tbl`, `iso_model.joblib`

**Training pipeline (design finalized):**
1. Parse log -> DataFrame
2. Cast LightLevel -> int, ISO -> int, Scene -> str
3. Train `DecisionTreeClassifier(max_depth=4)` on `[LightLevel] -> ISO`
4. Generate LUT rows using ISO model prediction + integer threshold rules:
   - ETTR: `light > 200 -> reduce`, `light < 30 -> increase`, else `keep`
   - ALO: `light < 20 -> shadow_boost`, `light < 50 -> shadow_lift`, else `neutral`
   - HTP: `light < 30 -> priority_on`, else `priority_off`
   - WB: scene-name lookup table (5 scenes, hardcoded integer multipliers)
5. Sort output by Scene then LightLevel
6. Write `unified.tbl` with comment header

**Integer alignment:** All threshold constants in Python (200, 30, 20, 50)
are the same integers used in the Lua decision engine. There is no translation
layer -- the training and the on-camera lookup share the same numeric space.

---

### Component 5: GitHub Actions Workflows

**`build.yml`:**
- Trigger: push/PR to `ai-lut-integration`, nightly `0 2 * * *`
- Matrix: `[6D.116, 5D3.113, 60D.111, 650D.104]`
- Steps: checkout, install `gcc-arm-none-eabi`, `make clean && make || true`
- Artifacts: `magiclantern-{camera}/magiclantern.bin`, `unified-lut/unified.tbl`

**`release.yml`:**
- Trigger: `workflow_run` on build completion
- Creates GitHub Release `v{run_number}` with per-camera folders

---

## Data Models

### Log entry (text, SD card)

```
Timestamp  = ISO 8601 string    e.g. "2026-07-16T14:07:00"
Hist       = 256 comma-sep ints e.g. "0,0,12,45,..."
Scene      = string             e.g. "daylight"
LightLevel = integer 0-255      computed from 90th-percentile histogram bin
Shutter    = string fraction    e.g. "1/100"
ISO        = integer            e.g. 400
WB         = string             e.g. "R120,G100,B90"
---                             entry separator
```

### LUT row (unified.tbl)
```
Scene|LightLevel|ETTR|ALO|HTP|ISO|WB
```

### Lua in-memory model table
```lua
model["daylight_80"] = {
    ETTR       = "reduce_shutter",
    ALO        = "shadow_lift",
    HTP        = "priority_off",
    ISO        = "100",
    WB         = "R120,G100,B90",
    LightLevel = 80   -- stored as integer for nearest-neighbor distance calc
}
```

---

## Error Handling

### On-Camera (Lua)

| Failure | Handling |
|---|---|
| `unified.tbl` not found | Use fallback defaults; log `LUT_NOT_FOUND` |
| LUT key not found | Nearest-neighbor search; if still nil use fallback; log `FALLBACK_USED` |
| `io.open` returns nil | Skip logging silently; no crash |
| `get_histogram()` returns nil | Log `HIST_NIL\|LightLevel=128(fallback)` — combined entry distinguishes missing data from real mid-tone scenes |
| WB parse fails | Call `set_wb(100,100,100)` neutral fallback; log `WB_PARSE_ERROR\|raw={value}\|fallback=R100,G100,B100` |
| `tonumber(iso)` returns nil or 0 | Use fallback ISO 400; log `ISO_PARSE_ERROR` |
| Half-press takes > 100 ms | No timeout mechanism in Lua; mitigated by keeping loop < 256 iterations and single file open/close |

### Offline Training (Python)

| Failure | Handling |
|---|---|
| Empty log file | Raise `ValueError` with clear message |
| Missing LightLevel or ISO column | Skip row with warning print |
| < 5 training samples | Print warning; training still proceeds |

### CI/CD

| Failure | Handling |
|---|---|
| `make` fails for one camera | `|| true` allows matrix to continue; failed camera artifact is absent from release |
| Release artifact missing | `|| true` in copy step; release created with available files and tagged `[PARTIAL]` in title |
| All cameras fail | Release is still created but body clearly lists which cameras have no binary |

**Partial release labeling:** When `release.yml` runs, it checks which
`magiclantern-{camera}` artifacts were actually downloaded. If any are missing,
it prefixes the release title with `[PARTIAL]` and lists the missing cameras
in the release body. This ensures testers immediately see what is and is not
included, rather than discovering missing binaries after download.

```yaml
# Pseudocode for release.yml partial detection
- name: Check for missing artifacts
  run: |
    MISSING=""
    for cam in 6D.116 5D3.113 60D.111 650D.104; do
      if [ ! -f "artifacts/magiclantern-$cam/magiclantern.bin" ]; then
        MISSING="$MISSING $cam"
      fi
    done
    echo "MISSING_CAMS=$MISSING" >> $GITHUB_ENV
    if [ -n "$MISSING" ]; then
      echo "RELEASE_PREFIX=[PARTIAL]" >> $GITHUB_ENV
    else
      echo "RELEASE_PREFIX=" >> $GITHUB_ENV
    fi
```

---

## Correctness Properties

### Property 1: Integer purity on camera. **Validates: Requirements 3**
Every on-camera operation uses only integer values. The Lua scripts contain no decimal constants, no `/` division producing fractions, and no `math.sqrt` or trigonometric calls. The 90th-percentile histogram calculation uses one `math.floor` call which is acceptable (returns an integer).

### Property 2: Threshold alignment between training and inference. **Validates: Requirements 3.4**
The ETTR threshold constants (200, 30), ALO thresholds (20, 50), and HTP threshold (30) used in `lut_training_multi_param.py` are identical integers to those in REQ-003 and REQ-004. The LUT encodes the output of these thresholds; the Lua engine only does a table lookup — it never re-evaluates thresholds at runtime.

### Property 3: WB multipliers are final camera values. **Validates: Requirements 3**
The LUT's WB field contains the exact integer arguments for `set_wb()`. No conversion, no scaling, no Kelvin math on camera. G=100 is always the reference. On WB parse failure, neutral `R100,G100,B100` is always applied — the camera is never left in an undefined WB state.

### Property 4: Missing LUT never crashes. **Validates: Requirements 3**
`load_unified_model` returns an empty table if the file is missing or unreadable. `find_decision` returns nil on an empty table. The fallback defaults block catches nil and provides safe values. No error propagates to the camera hardware calls.

### Property 5: LUT hot-swap takes effect immediately. **Validates: Requirements 6**
A retrained `unified.tbl` copied to `A:/ML/models/` on the SD card takes effect on the very next half-press shutter event. `load_unified_model` is called fresh on every half-press — there is no cache to invalidate.

### Property 6: Training script output is deterministic. **Validates: Requirements 4**
`lut_training_multi_param.py` run twice on the same `unified_log.txt` produces byte-identical `unified.tbl` output (fixed random seed, sorted output).

---

## Testing Strategy

### On-Camera Testing
1. Deploy `unified_logger.lua` only. Capture 5 half-presses under different
   lighting. Verify `unified_log.txt` is created with correct LightLevel
   (90th-percentile, integer).
2. Deploy `decision_engine.lua` with seed `unified.tbl`. Capture shots in
   daylight, tungsten, shade, lowlight. Verify CR2 metadata via `exiftool`:
   `exiftool IMG.CR2 | grep -E "ISO|Shutter|White Balance"`
3. Remove `unified.tbl` from SD card. Verify fallback defaults are applied
   and `FALLBACK_USED` appears in log.
4. Replace `unified.tbl` with a new version mid-session (without rebooting).
   Verify new decisions take effect on next half-press (hot-swap test).

### Offline Training Testing
1. Run `lut_training_multi_param.py` on `sample_unified_log.txt`. Verify
   output `unified.tbl` format matches REQ-002.
2. Run twice. Verify output is byte-identical (determinism check).
3. Verify ETTR decisions: all rows with LightLevel > 200 have `reduce_shutter`.
4. Verify WB: all `tungsten` rows have `R150,G100,B70`.

### CI Testing
1. Push to `ai-lut-integration`. Verify all 4 matrix jobs appear in Actions.
2. Confirm artifact names: `magiclantern-{camera}` and `unified-lut`.
3. Verify release is created after successful build.

### CI/CD

| Failure | Handling |
|---|---|
| `make` fails for one camera | `|| true` allows matrix to continue; failed camera artifact is absent from release |
| Release artifact missing | `|| true` in copy step; release created with available files and tagged `[PARTIAL]` in title |
| All cameras fail | Release still created; body explicitly lists which cameras have no binary |

**Partial release labeling:** When `release.yml` runs, it checks which
`magiclantern-{camera}` artifacts were downloaded. If any are missing, the
release title is prefixed with `[PARTIAL]` and the release body lists the
missing cameras. This ensures testers immediately know what is and is not
included, rather than discovering missing binaries after download.

```yaml
# Pseudocode for release.yml partial detection
- name: Check for missing artifacts
  run: |
    MISSING=""
    for cam in 6D.116 5D3.113 60D.111 650D.104; do
      if [ ! -f "artifacts/magiclantern-$cam/magiclantern.bin" ]; then
        MISSING="$MISSING $cam"
      fi
    done
    if [ -n "$MISSING" ]; then
      echo "RELEASE_TITLE=[PARTIAL] AI Magic Lantern Build ${{ github.run_number }}" >> $GITHUB_ENV
      echo "MISSING_NOTE=Missing camera builds:$MISSING" >> $GITHUB_ENV
    fi
```

---

## Testing Strategy

### On-Camera Testing
1. Deploy `unified_logger.lua` only. Capture 5 half-presses under different
   lighting. Verify `unified_log.txt` is created with correct LightLevel
   (90th-percentile, integer).
2. Deploy `decision_engine.lua` with seed `unified.tbl`. Capture shots in
   daylight, tungsten, shade, lowlight. Verify CR2 metadata via `exiftool`:
   `exiftool IMG.CR2 | grep -E "ISO|Shutter|White Balance"`
3. Remove `unified.tbl` from SD card. Verify fallback defaults are applied
   and `FALLBACK_USED` appears in log.
4. Replace `unified.tbl` with a new version mid-session (without rebooting).
   Verify new decisions take effect on next half-press (hot-swap test).
5. Feed a malformed WB string into `apply_wb`. Verify `set_wb(100,100,100)`
   is called and `WB_PARSE_ERROR` appears in log.

### Offline Training Testing
1. Run `lut_training_multi_param.py` on `sample_unified_log.txt`. Verify
   output `unified.tbl` format matches REQ-002.
2. Run twice. Verify output is byte-identical (determinism check).
3. Verify ETTR decisions: all rows with LightLevel > 200 have `reduce_shutter`.
4. Verify WB: all `tungsten` rows have `R150,G100,B70`.

### CI Testing
1. Push to `ai-lut-integration`. Verify all 4 matrix jobs appear in Actions.
2. Confirm artifact names: `magiclantern-{camera}` and `unified-lut`.
3. Verify release is created after successful build.
4. Simulate a partial build (one camera fails). Verify release title contains
   `[PARTIAL]` and body lists the missing camera model.
