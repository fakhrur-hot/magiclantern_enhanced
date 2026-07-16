# Implementation Plan: AI-Assisted Exposure and Color for Magic Lantern

## Overview

Implementation is organized into 12 tasks across four phases:

- **Phase 1 -- Foundation (Tasks 1-2):** Repository structure, GitHub remote,
  and seed LUT file.
- **Phase 2 -- Core Lua Pipeline (Tasks 3-4):** On-camera logger and decision
  engine scripts. This is the highest-value phase -- all on-camera intelligence
  lives here.
- **Phase 3 -- Offline Training + CI/CD (Tasks 5-8):** Colab notebooks,
  training script, and GitHub Actions workflows. Training design is finalized;
  these tasks are wiring and configuration.
- **Phase 4 -- Docs, Validation, and Extensions (Tasks 9-12):** Documentation,
  integration testing, ALO/HTP hooks.

All on-camera logic (Tasks 3-4) runs on the EOS 6D ARM Cortex-R4 in Lua.
Integer-only math. No GPU. No FPU. No floating-point constants.

---

## Tasks

- [ ] 1. Set up repository structure and GitHub remote connection
  - Create top-level directories: `lua_scripts/`, `colab/`, `models/`, `logs/`,
    `docs/`, `release_templates/`, `.github/workflows/`
  - Add skeleton files: `README.md`, `INSTALL.md`, `ARCHITECTURE.md`,
    `CONTRIBUTING.md`, `LICENSE` (MIT)
  - Initialize git with `ai-lut-integration` as the working branch
  - Connect remote: `git@github.com:fakhrur-hot/magiclantern_enhanced.git`
  - Add `.gitignore` excluding `*.pyc`, `__pycache__/`, `*.joblib`,
    `logs/*.txt` (do not commit raw SD card log data)
  - Push initial skeleton commit to `ai-lut-integration`
  - _Requirements: REQ-008, REQ-009_

- [ ] 2. Create the unified LUT seed file and sample log
  - Write `models/unified.tbl` with the 5-scene seed rows:
    ```
    # Scene|LightLevel|ETTR|ALO|HTP|ISO|WB
    daylight|80|reduce_shutter|shadow_lift|priority_off|100|R120,G100,B90
    tungsten|20|increase_shutter|shadow_boost|priority_on|1600|R150,G100,B70
    shade|60|keep_shutter|neutral|priority_off|400|R110,G100,B105
    lowlight|10|increase_shutter|shadow_boost|priority_on|3200|R130,G100,B80
    unknown|128|keep_shutter|neutral|priority_off|400|R100,G100,B100
    ```
  - Validate: UTF-8, no BOM, file size <= 1 MB, all WB values have G=100
  - Create `logs/sample_unified_log.txt` with 5 representative log entries
    (key=value format, `---` separator, LightLevel as integer 0-255)
  - _Requirements: REQ-002_

- [ ] 3. Implement `lua_scripts/unified_logger.lua`
  - Implement `compute_light_level(hist)` using integer-only 90th-percentile
    algorithm (see design doc Component 1):
    - `total` = sum of all 256 bins (integer addition loop)
    - `threshold` = `math.floor(total * 9 / 10)` (one floor call, integer result)
    - Walk bins until cumulative >= threshold; return 0-based bin index
    - Fallback: return 255 if loop completes without crossing threshold
  - Implement `get_histogram_string()` using ML `get_histogram()` API;
    return `"nil"` if API returns nil
  - Implement `append_log(path, entry)` using `io.open(path, "a")`;
    handle nil file gracefully (silent skip, no crash)
  - Implement `half_press_logger()` callback:
    - Calls `compute_light_level()` on histogram bins
    - Formats log entry as key=value lines with `---` separator
    - Appends to `A:/ML/logs/unified_log.txt`
  - Register: `register_half_press_callback(half_press_logger)`
  - Verify: no `set_*` camera API calls anywhere in the script
  - Verify: no floating-point constants or division producing fractions
  - Verify: script size <= 10 KB
  - _Requirements: REQ-001_

- [ ] 4. Implement `lua_scripts/decision_engine.lua`
  - Implement `compute_light_level(hist)` -- identical to logger (shared logic,
    duplicated across scripts to keep each script self-contained and < 10 KB)
  - Implement `load_unified_model(path)`:
    - `io.open(path, "r")`; return empty table if file missing
    - Skip lines where `string.sub(line,1,1) == "#"` or no `"|"` found
    - Parse 7-column pipe-split; store in table keyed by `scene.."_"..light`
    - Store `LightLevel` as integer in row for nearest-neighbor distance calc
  - Implement `find_decision(model, scene, light_level)`:
    - Exact match first: `model[scene.."_"..tostring(light_level)]`
    - Nearest-neighbor: integer `abs(row.LightLevel - light_level)` loop
      over matching scene rows (string prefix check)
    - Scene fallback: if no match, try `"unknown"` scene nearest-neighbor
    - Return nil if model is empty (handled by caller)
  - Implement `apply_ettr(ettr)`:
    - `"reduce_shutter"` -> `set_shutter_fraction(1, 200)`
    - `"keep_shutter"` -> `set_shutter_fraction(1, 100)`
    - `"increase_shutter"` -> `set_shutter_fraction(1, 50)`
    - Default: `set_shutter_fraction(1, 100)` (same as keep, silent)
  - Implement `apply_iso(iso_str)`:
    - `local n = tonumber(iso_str)`
    - Guard: if `not n or n == 0` then `n = 400`; log `ISO_PARSE_ERROR`
    - `set_iso(n)`
  - Implement `apply_wb(wb_str)`:
    - `string.match(wb_str, "R(%d+),G(%d+),B(%d+)")`
    - If all three match: `set_wb(tonumber(r), tonumber(g), tonumber(b))`
    - Else: log `WB_PARSE_ERROR|<wb_str>`, skip `set_wb()`
  - Implement `apply_alo(alo)` and `apply_htp(htp)` as `-- TODO` stubs
    (documented no-ops; bodies left empty but functions present and called)
  - Implement `log_decision(decision, scene, light)`:
    - Appends full decision record to `unified_log.txt` (same format as logger)
  - Implement `half_press_decision()`:
    - Get histogram, compute light level (integer)
    - Load model, find decision
    - If nil: use FALLBACK table, log `FALLBACK_USED`
    - Apply all five decisions (ettr, iso, wb, alo, htp)
    - Log decision
  - Register: `register_half_press_callback(half_press_decision)`
  - Verify: all math is integer-only. No `/` producing fractions.
    No floating-point constants. `math.floor` only in `compute_light_level`.
  - Verify: script size <= 10 KB
  - _Requirements: REQ-003_

- [ ] 5. Implement `colab/lut_training.ipynb`
  - Cell 1: upload `unified_log.txt` via `google.colab.files.upload()`
  - Cell 2: parse log into Pandas DataFrame (key=value, `---` separator)
  - Cell 3: feature engineering -- `LightLevel` to int, `ISO` to int,
    `Scene` to str
  - Cell 4: train `DecisionTreeClassifier(max_depth=4)` on `[LightLevel] -> ISO`
    with `random_state=42` for determinism
  - Cell 5: generate LUT rows using integer threshold rules:
    - ETTR: `> 200 -> reduce_shutter`, `< 30 -> increase_shutter`, else `keep`
    - ALO: `< 20 -> shadow_boost`, `< 50 -> shadow_lift`, else `neutral`
    - HTP: `< 30 -> priority_on`, else `priority_off`
    - WB: scene-to-integer-multiplier dict (5 scenes, G=100 reference)
  - Cell 6: sort rows by scene then LightLevel; write `unified.tbl`;
    `files.download("unified.tbl")`
  - Validate: runs end-to-end on `sample_unified_log.txt` without errors
  - _Requirements: REQ-004_

- [ ] 6. Implement `colab/lut_training_multi_param.py`
  - Replicate notebook logic as standalone CLI Python script
  - Read `unified_log.txt` from CWD; write `unified.tbl` to CWD
  - Save `iso_model.joblib` for incremental retraining
  - `random_state=42` in DecisionTreeClassifier for determinism
  - Sort output rows: Scene alphabetically, then LightLevel numerically
  - Validate: output `unified.tbl` is byte-identical to notebook for same input
  - _Requirements: REQ-004_

- [ ] 7. Implement `.github/workflows/build.yml`
  - Triggers: push/PR to `ai-lut-integration`, nightly `0 2 * * *`
  - Matrix: `camera: [6D.116, 5D3.113, 60D.111, 650D.104]`
  - Steps: checkout, `apt-get install gcc-arm-none-eabi make`,
    `make clean && make || true` in `platform/${{ matrix.camera }}`
  - Upload `magiclantern.bin` as artifact `magiclantern-{camera}`
  - Upload `models/unified.tbl` as artifact `unified-lut`
  - _Requirements: REQ-005_

- [ ] 8. Implement `.github/workflows/release.yml`
  - Trigger: `workflow_run` on completion of `Build AI Magic Lantern`
  - Download all artifacts; organize into `release/EOS-{model}/`
  - Copy `unified.tbl` into each camera folder
  - Create GitHub Release `v${{ github.run_number }}` using
    `softprops/action-gh-release@v1`
  - Body from `release_templates/ReleaseNotes.md`
  - _Requirements: REQ-005_

- [ ] 9. Write documentation files
  - `docs/VALIDATION.md`: exiftool validation steps, ISO/WB match rate metrics,
    ETTR histogram check (90th-percentile bin in 180-240 after reduce_shutter)
  - `docs/TROUBLESHOOTING.md`: LUT not loaded, wrong ISO/WB in CR2, build failures,
    debug logging to `A:/ML/logs/debug.txt`
  - `docs/CI_CD.md`: workflow diagram, manual trigger, add new camera to matrix
  - `ARCHITECTURE.md`: full data flow (Camera -> Colab -> SD card -> CI -> Camera)
  - `README.md`: glossary section with all key terms
  - _Requirements: REQ-007, REQ-008_

- [ ] 10. Integration validation
  - Deploy `unified_logger.lua` + seed `unified.tbl` to test SD card
  - Capture 10 half-press events; verify `unified_log.txt` populated with
    correct integer LightLevel (compare to visual scene brightness)
  - Deploy `decision_engine.lua`; capture shots in daylight, tungsten,
    shade, lowlight; verify CR2 metadata with exiftool
  - Run `lut_training_multi_param.py` on exported log -> retrained `unified.tbl`
  - Deploy retrained LUT (hot-swap); capture second round; verify new decisions
  - Document ISO match rate (%), WB match rate (%), ETTR histogram result
    in `docs/VALIDATION.md`
  - Fix any Lua API mismatches found during on-camera testing
  - _Requirements: REQ-006, REQ-007_

- [ ] 11. Implement ALO tone curve hooks (post-Phase 1)
  - Research available Magic Lantern tone curve API for EOS 6D
  - Replace `apply_alo(alo)` stub:
    - `shadow_lift` -> gentle shadow curve lift via ML tone API
    - `shadow_boost` -> stronger lift
    - `neutral` -> no-op (existing behavior)
  - All API calls must be integer arguments; no floating-point parameters
  - Test on EOS 6D; extend to other matrix cameras as APIs permit
  - _Requirements: REQ-003 (ALO completion)_

- [ ] 12. Implement HTP highlight tone priority hooks (post-Phase 1)
  - Research available Magic Lantern HTP firmware hooks for EOS 6D
  - Replace `apply_htp(htp)` stub:
    - `priority_on` -> enable HTP via ML firmware hook
    - `priority_off` -> disable
  - Validate: capture high-DR scene; compare histogram clipping with HTP
    on vs off using exiftool + histogram analysis
  - _Requirements: REQ-003 (HTP completion)_

---

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 1, "tasks": ["Task 1"] },
    { "wave": 2, "tasks": ["Task 2"], "dependsOn": ["Task 1"] },
    { "wave": 3, "tasks": ["Task 3", "Task 4", "Task 5", "Task 7", "Task 9"],
      "dependsOn": ["Task 2"] },
    { "wave": 4, "tasks": ["Task 6"], "dependsOn": ["Task 5"] },
    { "wave": 4, "tasks": ["Task 8"], "dependsOn": ["Task 7"] },
    { "wave": 5, "tasks": ["Task 10"], "dependsOn": ["Task 3", "Task 4", "Task 6"] },
    { "wave": 6, "tasks": ["Task 11", "Task 12"], "dependsOn": ["Task 4", "Task 10"] }
  ]
}
```

---

## Notes

- **Tasks 3 and 4 are the core of this project.** Everything else is
  infrastructure. The quality of the on-camera Lua -- particularly
  `compute_light_level()` and `find_decision()` -- determines how well
  the AI decisions match real shooting conditions.
- **No floating point on camera.** Review every line of Tasks 3 and 4 for
  any accidental float: string formatting with `%.2f`, `math.sqrt`, `/`
  producing fractions. These will either silently produce wrong values or
  crash on DIGIC 5+ ARM Cortex-R4.
- **Training design is finalized (Tasks 5-6).** The thresholds (200/30/20/50)
  and WB multiplier table are not open questions -- they are established
  values. Task 5 and 6 are wiring, not research.
- **Tasks 11 and 12** (ALO/HTP hooks) are explicitly post-Phase 1 and depend
  on finding supported Magic Lantern API hooks per camera model. They must
  not block the initial release.
- Always test Lua script changes on a spare SD card before deploying to the
  primary card.
- The `ai-lut-integration` branch must never be force-pushed once CI is active.
