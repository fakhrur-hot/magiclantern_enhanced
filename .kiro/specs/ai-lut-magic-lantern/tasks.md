# Implementation Plan: AI-Assisted Exposure and Color for Magic Lantern

## Overview

Implementation is organized into 12 tasks across four phases:

- **Phase 1 — Foundation (Tasks 1–2):** Repository structure, GitHub remote, and seed LUT file.
- **Phase 2 — Core Lua Pipeline (Tasks 3–4):** On-camera logger and decision engine scripts.
- **Phase 3 — Offline Training + CI/CD (Tasks 5–8):** Colab notebooks, training script, and GitHub Actions workflows.
- **Phase 4 — Docs, Validation, and Extensions (Tasks 9–12):** Documentation, integration testing, ALO/HTP hooks.

Each task maps to one or more requirements. Tasks within a phase are largely independent and can be worked in parallel.

---

## Tasks

- [ ] 1. Set up repository structure and GitHub remote connection
  - Create top-level directories: `lua_scripts/`, `colab/`, `models/`, `logs/`, `docs/`, `release_templates/`, `.github/workflows/`
  - Add skeleton files: `README.md`, `INSTALL.md`, `ARCHITECTURE.md`, `CONTRIBUTING.md`, `LICENSE` (MIT)
  - Initialize git with `ai-lut-integration` as the working branch
  - Connect remote to `git@github.com:fakhrur-hot/magiclantern_enhanced.git` (SSH key or PAT with `repo`, `workflow` scopes)
  - Add `.gitignore` excluding `*.pyc`, `__pycache__/`, `*.joblib`, `logs/*.txt` (to avoid committing raw log data)
  - Push initial skeleton commit to `ai-lut-integration` (never to `main`/`master`)
  - _Requirements: REQ-008, REQ-009_

- [ ] 2. Create the unified LUT seed file and sample log
  - Write `models/unified.tbl` with pipe-delimited rows: `Scene|LightLevel|ETTR|ALO|HTP|ISO|WB`
  - Include comment header line starting with `#`
  - Include seed rows for `daylight`, `tungsten`, `shade`, `lowlight` at representative light levels (10, 20, 60, 80)
  - Validate: UTF-8 encoding, no BOM, file size ≤ 1 MB
  - Create `logs/sample_unified_log.txt` with 5 representative log entries (key=value format, `---` separator)
  - _Requirements: REQ-002_

- [ ] 3. Implement `lua_scripts/unified_logger.lua`
  - Implement `append_log(path, entry)` using `io.open` in append mode; handle nil file gracefully
  - Implement `get_histogram_string()` using ML `get_histogram()` API; return `"nil"` if API returns nil
  - Implement `half_press_logger()` callback reading: timestamp, histogram, scene, light level, ISO, WB, shutter
  - Format log entry as key=value lines with `---` separator
  - Log path: `A:/ML/logs/unified_log.txt`
  - Register callback with `register_half_press_callback(half_press_logger)`
  - Verify: no `set_*` camera API calls present (logging only)
  - Verify: script size ≤ 10 KB
  - _Requirements: REQ-001_

- [ ] 4. Implement `lua_scripts/decision_engine.lua`
  - Implement `load_unified_model(path)`: parse `unified.tbl` line by line; skip `#` comments and lines without `|`; return empty table if file missing
  - Build lookup table keyed by `scene .. "_" .. tostring(light_level)`
  - Implement `apply_ettr(ettr)`: map `reduce_shutter → set_shutter_fraction(1,200)`, `keep_shutter → (1,100)`, `increase_shutter → (1,50)`
  - Implement `apply_iso(iso)`: call `set_iso(tonumber(iso))`; guard against nil
  - Implement `apply_wb(wb)`: parse `R{n},G{n},B{n}` pattern; call `set_wb(r,g,b)`; log `WB_PARSE_ERROR` on failure
  - Implement `apply_alo(alo)` and `apply_htp(htp)` as documented `-- TODO` placeholders
  - Implement `log_decision(...)` reusing the same log format as `unified_logger.lua`
  - Implement `half_press_decision()`: load → lookup → apply all params → log
  - Apply fallback defaults when key not found; log `"FALLBACK_USED"`
  - Register callback with `register_half_press_callback(half_press_decision)`
  - Verify: all math is integer-only
  - Verify: script size ≤ 10 KB
  - _Requirements: REQ-003_

- [ ] 5. Implement `colab/lut_training.ipynb`
  - Cell 1: upload `unified_log.txt` via `google.colab.files.upload()`
  - Cell 2: parse log into Pandas DataFrame (key=value line split, `---` entry separator)
  - Cell 3: feature engineering — cast `LightLevel` to int, `ISO` to int, `Scene` to string
  - Cell 4: train `DecisionTreeClassifier(max_depth=4)` on `[LightLevel] → ISO`
  - Cell 5: generate LUT rows for each unique Scene + LightLevel using ISO model + rule-based thresholds for ETTR/ALO/HTP/WB
  - Cell 6: write `unified.tbl` and call `files.download("unified.tbl")`
  - Validate: notebook runs end-to-end on `sample_unified_log.txt` without errors
  - _Requirements: REQ-004_

- [ ] 6. Implement `colab/lut_training_multi_param.py`
  - Replicate notebook logic as a standalone CLI Python script
  - Read `unified_log.txt` from current working directory
  - Write `unified.tbl` to current working directory
  - Save `iso_model.joblib` for incremental retraining
  - Validate: output `unified.tbl` is identical to notebook output given same input (determinism check)
  - _Requirements: REQ-004_

- [ ] 7. Implement `.github/workflows/build.yml`
  - Configure triggers: push/PR to `ai-lut-integration`, nightly schedule `0 2 * * *`
  - Define matrix: `camera: [6D.116, 5D3.113, 60D.111, 650D.104]`
  - Steps: checkout repo, install `gcc-arm-none-eabi` via apt, run `make clean && make` in `platform/${{ matrix.camera }}` (use `|| true` to allow partial failures)
  - Upload `magiclantern.bin` as artifact `magiclantern-{camera}`
  - Upload `models/unified.tbl` as artifact `unified-lut`
  - Validate: push a commit and verify all 4 matrix jobs appear in Actions tab
  - _Requirements: REQ-005_

- [ ] 8. Implement `.github/workflows/release.yml`
  - Configure trigger: `workflow_run` on completion of `Build AI Magic Lantern`
  - Steps: download all artifacts, organize into `release/EOS-{model}/` folders
  - Copy `unified.tbl` into each camera folder
  - Create GitHub Release using `softprops/action-gh-release@v1` with tag `v${{ github.run_number }}`
  - Populate release body from `release_templates/ReleaseNotes.md`
  - Validate: trigger a build and confirm a GitHub Release is created with expected files
  - _Requirements: REQ-005_

- [ ] 9. Write documentation files
  - `docs/VALIDATION.md`: step-by-step `exiftool` validation, ISO/WB match rate metrics, ETTR histogram check
  - `docs/TROUBLESHOOTING.md`: LUT not loaded, wrong ISO/WB in CR2, build failures, debug logging instructions (`A:/ML/logs/debug.txt`)
  - `docs/CI_CD.md`: workflow diagram, how to trigger builds manually, how to add a new camera to the matrix
  - `ARCHITECTURE.md`: full data flow diagram (Camera → Colab → SD card → CI → Release → Camera)
  - `README.md`: add glossary section covering ML, Lua, LUT, ETTR, ALO, HTP, ISO, WB, CR2, CI/CD, Repo, Artifact, Upstream, Fork, Colab, Decision Tree
  - _Requirements: REQ-007, REQ-008_

- [ ] 10. Integration validation
  - Deploy `unified_logger.lua` + seed `unified.tbl` to a test SD card; capture 10 half-press events; verify `unified_log.txt` is populated
  - Deploy `decision_engine.lua`; capture shots in daylight, tungsten, shade, low-light; export log
  - Run `lut_training_multi_param.py` on exported log to produce retrained `unified.tbl`
  - Deploy retrained LUT; capture second round of shots
  - Use `exiftool IMG_XXXX.CR2 | grep -E "ISO|Shutter|White Balance"` to verify CR2 metadata matches log entries
  - Document ISO match rate (%), WB match rate (%), ETTR histogram result in `docs/VALIDATION.md`
  - Fix any Lua API mismatches found during on-camera testing
  - _Requirements: REQ-006, REQ-007_

- [ ] 11. Implement ALO tone curve hooks (post-Phase 1)
  - Research available Magic Lantern tone curve API for EOS 6D
  - Replace `apply_alo(alo)` placeholder: `shadow_lift` → lift shadow curve, `shadow_boost` → stronger lift, `neutral` → no-op
  - Test on EOS 6D; extend to other matrix cameras as APIs permit
  - Update `lut_training_multi_param.py` to support finer ALO gradations based on log data
  - _Requirements: REQ-003 (ALO completion)_

- [ ] 12. Implement HTP highlight tone priority hooks (post-Phase 1)
  - Research available Magic Lantern HTP firmware hooks for EOS 6D
  - Replace `apply_htp(htp)` placeholder: `priority_on` → enable HTP, `priority_off` → disable
  - Validate highlight clipping behavior in CR2 RAW using `exiftool` + histogram analysis
  - _Requirements: REQ-003 (HTP completion)_

---

## Task Dependency Graph

```json
{
  "waves": [
    { "wave": 1, "tasks": ["Task 1"] },
    { "wave": 2, "tasks": ["Task 2"], "dependsOn": ["Task 1"] },
    { "wave": 3, "tasks": ["Task 3", "Task 4", "Task 5", "Task 7", "Task 9"], "dependsOn": ["Task 2"] },
    { "wave": 4, "tasks": ["Task 6"], "dependsOn": ["Task 5"] },
    { "wave": 4, "tasks": ["Task 8"], "dependsOn": ["Task 7"] },
    { "wave": 5, "tasks": ["Task 10"], "dependsOn": ["Task 3", "Task 4", "Task 6"] },
    { "wave": 6, "tasks": ["Task 11", "Task 12"], "dependsOn": ["Task 4", "Task 10"] }
  ]
}
```

---

## Notes

- Tasks 3, 4, 5, 7, and 9 are independent of each other after Task 1 and 2 complete — they can be worked in parallel.
- Task 10 (integration validation) is the critical gate before any GitHub Release is considered production-ready.
- Tasks 11 and 12 (ALO/HTP hooks) are explicitly post-Phase 1 and depend on finding supported Magic Lantern API hooks per camera model. They should not block the initial release.
- Always test Lua script changes on a spare SD card before deploying to the primary card.
- CI build failures for individual cameras should use `|| true` to avoid blocking the entire matrix — partial releases are acceptable during early development.
- The `ai-lut-integration` branch should never be force-pushed once CI is active.
