# Design Document

## Overview

This document describes the technical architecture and component design for the AI-assisted exposure and color pipeline. The system has four layers: the on-camera Lua layer (EOS 6D), the offline training layer (Google Colab), the CI/CD layer (GitHub Actions), and the continuous improvement loop that connects them.

The core insight is that the EOS 6D's DIGIC 5+ ARM Cortex-R4 cannot run ML inference on-camera. All intelligence is computed offline and compressed into a compact LUT file (`unified.tbl`) stored on the SD card. On-camera Lua scripts perform only a table lookup and integer arithmetic, staying well within the 100 ms half-press budget.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        CAMERA LAYER (EOS 6D)                    │
│                                                                 │
│  Half-press shutter event                                       │
│         │                                                       │
│         ▼                                                       │
│  unified_logger.lua ──► A:/ML/logs/unified_log.txt             │
│         │                                                       │
│  decision_engine.lua                                            │
│         ├── load_unified_model("A:/ML/models/unified.tbl")     │
│         ├── lookup(scene + light_level)                        │
│         ├── apply_ettr() / apply_iso() / apply_wb()           │
│         └── log_decision() ──► A:/ML/logs/unified_log.txt     │
└─────────────────────────────────────────────────────────────────┘
                              │
                 SD card export (manual)
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                  OFFLINE TRAINING LAYER (Colab)                 │
│                                                                 │
│  unified_log.txt ──► parse_logs() ──► DataFrame                │
│         ├── Feature engineering (LightLevel, Scene)            │
│         ├── DecisionTreeClassifier (ISO model, max_depth=4)    │
│         ├── Rule-based ETTR / ALO / HTP thresholds             │
│         └── Export ──► unified.tbl                             │
└─────────────────────────────────────────────────────────────────┘
                              │
                 SD card copy + GitHub push
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                     CI/CD LAYER (GitHub Actions)                │
│                                                                 │
│  build.yml ──► matrix build (6D, 5D3, 60D, 650D)              │
│         └──► magiclantern.bin artifacts + unified.tbl          │
│                                                                 │
│  release.yml ──► GitHub Release with per-camera folders        │
└─────────────────────────────────────────────────────────────────┘
```

**Data flow:**
```
Camera (half-press) → log → SD card export → Colab train → unified.tbl
→ SD card copy + git push → CI build + release → decision_engine reads LUT
→ new logs collected → cycle repeats
```

---

## Components and Interfaces

### Component 1: `unified_logger.lua`

**Purpose:** Study-mode logging only. Captures sensor data during half-press. Does not alter any camera setting.

**Public interface (Magic Lantern callbacks):**

| Function | Signature | Description |
|---|---|---|
| `half_press_logger` | `() → void` | Main callback registered with `register_half_press_callback()` |

**Internal functions:**

| Function | Signature | Description |
|---|---|---|
| `append_log` | `(path: string, entry: string) → void` | Opens file in append mode, writes entry + newline, closes |
| `get_histogram_string` | `() → string` | Calls ML `get_histogram()`, serializes bins to comma-separated string |

**Log entry format (appended to `A:/ML/logs/unified_log.txt`):**
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

**Constraints:** Integer math only. No `set_*` API calls. Script ≤ 10 KB.

---

### Component 2: `decision_engine.lua`

**Purpose:** Full decision engine. Loads LUT on each half-press, applies decisions to camera, logs results.

**Public interface:**

| Function | Signature | Description |
|---|---|---|
| `half_press_decision` | `() → void` | Main callback registered with `register_half_press_callback()` |

**Internal functions:**

| Function | Signature | Description |
|---|---|---|
| `load_unified_model` | `(path: string) → table` | Parses `unified.tbl` into Lua table keyed by `"scene_lightlevel"` |
| `apply_ettr` | `(ettr: string) → void` | Maps keyword to `set_shutter_fraction(n, d)` |
| `apply_iso` | `(iso: string) → void` | Calls `set_iso(tonumber(iso))` |
| `apply_wb` | `(wb: string) → void` | Parses `R{n},G{n},B{n}` and calls `set_wb(r,g,b)` |
| `apply_alo` | `(alo: string) → void` | Placeholder — future tone curve hook |
| `apply_htp` | `(htp: string) → void` | Placeholder — future highlight tone priority toggle |
| `log_decision` | `(decision, hist, scene, light) → void` | Appends decision + sensor data to log file |

**ETTR → shutter mapping:**

| ETTR keyword | Camera call |
|---|---|
| `reduce_shutter` | `set_shutter_fraction(1, 200)` |
| `keep_shutter` | `set_shutter_fraction(1, 100)` |
| `increase_shutter` | `set_shutter_fraction(1, 50)` |

**Fallback defaults (no LUT match):**
```lua
{ ETTR="keep_shutter", ALO="neutral", HTP="priority_off", ISO="400", WB="R100,G100,B100" }
```

---

### Component 3: `unified.tbl` (LUT file)

**Location:** `A:/ML/models/unified.tbl` on SD card; `models/unified.tbl` in repository.

**Format:**
```
# Scene|LightLevel|ETTR|ALO|HTP|ISO|WB
daylight|80|reduce_shutter|shadow_lift|priority_off|100|R120,G100,B90
tungsten|20|increase_shutter|shadow_boost|priority_on|1600|R150,G100,B70
shade|60|keep_shutter|neutral|priority_off|400|R110,G100,B105
lowlight|10|increase_shutter|shadow_boost|priority_on|3200|R130,G100,B80
```

**Field constraints:**

| Field | Type | Valid values |
|---|---|---|
| Scene | string | `daylight`, `tungsten`, `shade`, `lowlight`, `unknown` |
| LightLevel | integer | 0–255 |
| ETTR | string | `reduce_shutter`, `keep_shutter`, `increase_shutter` |
| ALO | string | `shadow_lift`, `shadow_boost`, `neutral` |
| HTP | string | `priority_on`, `priority_off` |
| ISO | integer string | `100`, `200`, `400`, `800`, `1600`, `3200` |
| WB | string | `R{n},G{n},B{n}` where n is 0–255 |

---

### Component 4: `lut_training.ipynb` / `lut_training_multi_param.py`

**Interface:**
- Input: `unified_log.txt` (log file from SD card)
- Output: `unified.tbl` (ready for SD card deployment), `iso_model.joblib` (saved model)

**Training pipeline:**
1. Parse log → DataFrame
2. Cast `LightLevel` → int, `ISO` → int, `Scene` → string
3. Train `DecisionTreeClassifier(max_depth=4)` on `[LightLevel] → ISO`
4. Generate LUT rows using ISO model prediction + rule-based thresholds:
   - ETTR: `light > 200 → reduce`, `light < 30 → increase`, else `keep`
   - ALO: `light < 50 → shadow_lift`, else `neutral`
   - HTP: `light < 30 → priority_on`, else `priority_off`
   - WB: scene-based multipliers

---

### Component 5: GitHub Actions Workflows

**`build.yml` interface:**
- Trigger: push/PR to `ai-lut-integration`, nightly cron `0 2 * * *`
- Input: source tree at `platform/{camera}/`
- Output artifacts: `magiclantern-{camera}/magiclantern.bin`, `unified-lut/unified.tbl`
- Matrix: `[6D.116, 5D3.113, 60D.111, 650D.104]`

**`release.yml` interface:**
- Trigger: `workflow_run` on `build.yml` completion
- Input: all build artifacts
- Output: GitHub Release `v{run_number}` with per-camera folders containing `.bin` + `unified.tbl`

---

## Data Models

### Log Entry (text, SD card)

```
Timestamp = ISO 8601 string         e.g. "2026-07-16T14:07:00"
Hist      = comma-separated ints    e.g. "0,0,12,45,..."  (256 bins)
Scene     = string                  e.g. "daylight"
LightLevel = integer                e.g. 85
Shutter   = string fraction         e.g. "1/100"
ISO       = integer                 e.g. 400
WB        = string                  e.g. "R120,G100,B90"
---                                 entry separator
```

### LUT Row (unified.tbl)

```
Scene|LightLevel|ETTR|ALO|HTP|ISO|WB
```

### Lua model table (in-memory, after parsing)

```lua
model["daylight_80"] = {
    ETTR = "reduce_shutter",
    ALO  = "shadow_lift",
    HTP  = "priority_off",
    ISO  = "100",
    WB   = "R120,G100,B90"
}
```

### GitHub Release artifact layout

```
release/
  EOS-6D/
    magiclantern-6D.116.bin
    unified.tbl
  EOS-5D3/
    magiclantern-5D3.113.bin
    unified.tbl
  EOS-60D/
    magiclantern-60D.111.bin
    unified.tbl
  EOS-650D/
    magiclantern-650D.104.bin
    unified.tbl
```

---

## Error Handling

### On-Camera (Lua)

| Failure | Handling |
|---|---|
| `unified.tbl` not found | Use hardcoded fallback defaults; log `"LUT_NOT_FOUND"` to log file |
| LUT key not found | Use fallback defaults; log `"FALLBACK_USED"` with key attempted |
| `io.open` returns nil | Skip logging silently; no crash |
| `get_histogram()` returns nil | Log `"HIST_NIL"`, proceed with other fields |
| WB parse fails (bad format) | Skip `set_wb()` call; log `"WB_PARSE_ERROR"` |

### Offline Training (Python)

| Failure | Handling |
|---|---|
| Empty log file | Raise `ValueError` with clear message |
| Missing `LightLevel` or `ISO` column | Skip row with warning print |
| Model training with < 5 samples | Print warning; training still proceeds |

### CI/CD

| Failure | Handling |
|---|---|
| `make` fails for one camera | `|| true` allows matrix to continue; artifact is skipped |
| Release artifact missing | `|| true` in copy step; release still created with available files |

---

## Correctness Properties

### Property 1: ETTR shutter mapping is deterministic **Validates: Requirements REQ-003**
A Lua script that loads a well-formed `unified.tbl` and calls `apply_ettr("keep_shutter")` must result in `set_shutter_fraction(1, 100)` being called — no other shutter value is acceptable.

### Property 2: Fallback ISO is never nil or zero **Validates: Requirements REQ-003**
The fallback path must never call `set_iso(0)` or `set_iso(nil)`. The hardcoded fallback ISO string `"400"` must always produce `set_iso(400)`.

### Property 3: Missing LUT file never crashes the Lua script **Validates: Requirements REQ-003**
`load_unified_model` must never throw an error when the file is missing or unreadable. It must return an empty table, after which the fallback defaults take over.

### Property 4: LUT hot-swap works without firmware reflash **Validates: Requirements REQ-006**
A retrained `unified.tbl` copied to `A:/ML/models/` on the SD card must take effect on the very next half-press shutter event, with no camera reboot or firmware change required.

### Property 5: Training script is deterministic **Validates: Requirements REQ-004**
`lut_training_multi_param.py` run twice on the same `unified_log.txt` input must produce byte-identical `unified.tbl` output.

---

## Testing Strategy

### On-Camera Testing
1. Deploy `unified_logger.lua` only — verify `unified_log.txt` is created and populated after 5 half-presses.
2. Deploy `decision_engine.lua` with seed `unified.tbl` — capture shots in each scene condition and verify CR2 metadata via `exiftool`.
3. Remove `unified.tbl` from SD card — verify fallback defaults are applied and logged.

### Offline Training Testing
1. Run `lut_training_multi_param.py` on `sample_unified_log.txt` — verify `unified.tbl` output matches expected format.
2. Run twice — verify output is identical (determinism check).

### CI Testing
1. Push a commit to `ai-lut-integration` — verify all 4 matrix jobs appear in Actions tab.
2. Confirm artifact names match pattern `magiclantern-{camera}` and `unified-lut`.
3. Verify release is created after a successful build.

### Integration Test
1. Full cycle: camera logs → Colab retrain → new LUT → SD card → capture → `exiftool` compare.
2. ISO match rate ≥ 80%, WB match rate ≥ 80% on test shots.
