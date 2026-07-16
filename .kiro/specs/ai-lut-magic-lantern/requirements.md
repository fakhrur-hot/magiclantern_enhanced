# Requirements Document

## Introduction

This project builds an end-to-end AI-assisted exposure and color control pipeline
for Canon DSLRs running Magic Lantern. It combines on-camera Lua scripting,
offline model training in Google Colab, a unified LUT format, and CI/CD
automation to deliver optimized ETTR, ALO, HTP, ISO, and WB decisions before
each CR2 RAW capture.

The system is designed around the constraints of the EOS 6D''s DIGIC 5+ ARM
Cortex-R4 processor: **no on-camera ML inference, integer-only math, small
memory footprint, and SD card I/O for model storage.** All intelligence is
computed offline and compressed into a compact `unified.tbl` file. On-camera
Lua scripts perform only a table lookup and integer arithmetic -- no floating
point, no network, no GPU.

The offline training pipeline (Colab + `lut_training_multi_param.py`) is
**design-complete**. The `DecisionTreeClassifier(max_depth=4)` for ISO
prediction and the rule-based ETTR / ALO / HTP / WB thresholds are finalized.
REQ-004 implementation is treated as configuration work, not new research.

The GitHub remote repository is `fakhrur-hot/magiclantern_enhanced`. All AI
feature work lives on the `ai-lut-integration` branch.

---

## Requirements

### REQ-001 -- On-Camera Sensor Data Logging

**User Story:** As a photographer using Magic Lantern, I want the camera to
automatically log raw sensor data during every half-press shutter event, so
that I can collect a dataset for offline AI model training.

**Acceptance Criteria:**

- The Lua logging script hooks into the Magic Lantern half-press shutter event.
- Each log entry captures: timestamp, histogram bins (256 values), scene mode,
  light level, current ISO, current WB, and current shutter speed.
- Light level is derived from the histogram: the 90th-percentile bin index of
  the green channel (integer 0-255). This is the "near-highlight" cutoff --
  a robust ETTR signal that shows whether bright tones are near clipping.
- Entries are appended to `A:/ML/logs/unified_log.txt` on the SD card in
  key=value format with `---` as entry separator.
- The script runs without modifying any camera settings (study/logging mode only).
- Script size remains under 10 KB.
- Logging completes within 100 ms to maintain responsiveness.
- All arithmetic is integer-only (no floating-point operations in Lua).

---

### REQ-002 -- Unified LUT File Format

**User Story:** As a developer, I want a single compact LUT file that encodes
all AI decisions (ETTR, ALO, HTP, ISO, WB) per scene and light level, so that
the on-camera Lua script can look up all parameters in one read.

**Acceptance Criteria:**

- LUT file (`unified.tbl`) uses pipe-delimited rows: `Scene|LightLevel|ETTR|ALO|HTP|ISO|WB`.
- Lines starting with `#` are treated as comments and skipped.
- ETTR values are: `reduce_shutter`, `keep_shutter`, `increase_shutter`.
- ALO values are: `shadow_lift`, `shadow_boost`, `neutral`.
- HTP values are: `priority_on`, `priority_off`.
- ISO is a numeric string (e.g., `100`, `400`, `1600`, `3200`).
- WB is in RGB multiplier format: `R{n},G{n},B{n}` where n is 0-255 integer.
  G is always 100 (reference). R and B are scaled relative to G.
  Higher R relative to B = warmer scene. Stored as integers, applied directly
  via `set_wb(r,g,b)` -- no Kelvin conversion needed on camera.
- File size must remain <= 1 MB for fast SD card reads.
- File encoding is UTF-8 with no BOM.
- LUT rows are sorted by Scene then LightLevel ascending for deterministic
  nearest-neighbor lookup in Lua.

---

### REQ-003 -- On-Camera LUT Decision Engine

**User Story:** As a photographer, I want Magic Lantern to automatically load
the LUT and apply optimized ETTR, ISO, and WB settings before each shot, so
that my CR2 RAW files are consistently well-exposed.

**Acceptance Criteria:**

- The decision engine Lua script loads `unified.tbl` from `A:/ML/models/` on
  each half-press event.
- It derives the current scene and light level using the same integer method
  as the logger (90th-percentile histogram bin, integer 0-255).
- It looks up the current scene + light level key and retrieves ETTR, ALO,
  HTP, ISO, WB decisions.
- **Nearest-neighbor fallback:** if no exact key match exists, the engine
  finds the row with the same scene and smallest `abs(row_light - current_light)`
  difference using integer subtraction only. No floating point required.
- If still no match (scene not in LUT at all), safe fallback defaults apply:
  `keep_shutter`, `neutral`, `priority_off`, ISO 400, WB `R100,G100,B100`.
- **ETTR decision execution** (integer shutter denominators only):
  - `reduce_shutter`: `set_shutter_fraction(1, 200)` -- halves exposure,
    pulls histogram left to protect near-blown highlights.
  - `keep_shutter`: `set_shutter_fraction(1, 100)` -- neutral, no change.
  - `increase_shutter`: `set_shutter_fraction(1, 50)` -- doubles exposure,
    pushes histogram right to lift shadow detail.
  - Denominators 50/100/200 are integer constants, powers of 2 * 50.
    No division in Lua -- just a table lookup from keyword to integer pair.
- **ISO decision execution** (integer):
  - `set_iso(tonumber(iso_string))`. Guard: nil or zero -> fallback ISO 400.
  - LUT contains only valid Canon values: 100, 200, 400, 800, 1600, 3200.
- **WB decision execution** (integer string parse):
  - `string.match(wb, "R(%d+),G(%d+),B(%d+)")` extracts three integer strings.
  - `set_wb(tonumber(r), tonumber(g), tonumber(b))`.
  - Guard: if any channel is nil, apply neutral fallback `set_wb(100,100,100)`
    and log `WB_PARSE_ERROR|raw={value}|fallback=R100,G100,B100`.
    Camera must never be left in undefined WB state after a parse failure.
- **ALO** has a `-- TODO Phase 2` stub. Documented integer API target:
  `set_alo(2)` = shadow_boost, `set_alo(1)` = shadow_lift, `set_alo(0)` = neutral.
  No LUT format change needed when hooks are implemented.
- **HTP** has a `-- TODO Phase 2` stub. Documented integer API target:
  `set_htp(1)` = priority_on, `set_htp(0)` = priority_off.
  No LUT format change needed when hooks are implemented.
- `get_histogram()` returning nil must produce a single combined log entry:
  `HIST_NIL|LightLevel=128(fallback)` — this distinguishes missing histogram
  data from a genuine mid-tone scene (LightLevel ~128) in offline analysis.
- Every decision (applied or fallback) is logged to `unified_log.txt`.
- All math uses integer operations only. No `math.floor`, no `/`, no `*`
  with non-integer constants anywhere in the decision path.
- Script size remains under 10 KB.

---

### REQ-004 -- Offline LUT Training Pipeline (Google Colab)

**User Story:** As a developer, I want a Colab notebook that ingests camera
logs, trains a lightweight model, and exports a ready-to-deploy `unified.tbl`,
so that LUTs improve automatically as more log data is collected.

**Status: Model design finalized.** All thresholds and the ISO decision tree
are calibrated to EOS 6D DIGIC 5+ sensor characteristics. This requirement
covers implementation/wiring only.

**Acceptance Criteria:**

- Colab notebook parses `unified_log.txt` into a Pandas DataFrame.
- Feature engineering converts LightLevel to integer and Scene to string categories.
- A scikit-learn `DecisionTreeClassifier(max_depth=4)` is trained on
  `[LightLevel] -> ISO`. max_depth=4 gives at most 16 leaf nodes -- right for
  a 5-scene, continuous-light-level space without overfitting.
- **ETTR threshold rules** (must match REQ-003 integer shutter mapping):
  - `light_level > 200` -> `reduce_shutter`
  - `light_level < 30`  -> `increase_shutter`
  - else                -> `keep_shutter`
  - Threshold 200 = 90th-percentile bin where EOS 6D histogram signals
    near-clipping highlights. Threshold 30 = underexposed dark scene.
- **ALO threshold rules:**
  - `light_level < 20` -> `shadow_boost`  (very dark, aggressive lift)
  - `light_level < 50` -> `shadow_lift`   (moderately dark, gentle lift)
  - else               -> `neutral`
  - Check `< 20` first; `< 50` is the fallthrough.
- **HTP threshold rules:**
  - `light_level < 30` -> `priority_on`
  - else               -> `priority_off`
- **WB multiplier derivation** (scene-based, integer output, G=100 reference):
  - `daylight`:  R=120, G=100, B=90   (5500K natural light)
  - `shade`:     R=110, G=100, B=105  (6500K overcast / shadow)
  - `tungsten`:  R=150, G=100, B=70   (3200K incandescent)
  - `lowlight`:  R=130, G=100, B=80   (mixed warm sources)
  - `unknown`:   R=100, G=100, B=100  (neutral, no correction)
  - These are sensor gain multipliers, not Kelvin. Stored as integers.
    No conversion required on camera; passed directly to `set_wb()`.
- LUT rows are generated for every unique Scene + LightLevel in logs.
- Exported `unified.tbl` matches REQ-002 pipe-delimited format.
- A standalone Python script (`lut_training_multi_param.py`) replicates the
  notebook for automated runs with deterministic output (fixed seed, sorted rows).
- Both notebook and script produce identical `unified.tbl` for the same input.

---

### REQ-005 -- CI/CD Build Pipeline (GitHub Actions)

**User Story:** As a developer, I want automated GitHub Actions workflows that
build Magic Lantern firmware for multiple Canon camera models and publish
releases with firmware binaries and the latest LUT, so that deployments are
consistent and repeatable.

**Acceptance Criteria:**

- `build.yml` workflow triggers on push and PR to `ai-lut-integration` branch,
  and on a nightly schedule (02:00 UTC).
- Build matrix covers at minimum: `6D.116`, `5D3.113`, `60D.111`, `650D.104`.
- Each camera build runs `make clean && make` in `platform/<camera>/`.
- Compiled `magiclantern.bin` is uploaded as a named artifact per camera.
- `unified.tbl` is uploaded as a separate artifact.
- `release.yml` workflow triggers on completion of a successful `build.yml` run.
- Release job organizes artifacts into per-camera folders and creates a GitHub
  Release with a version tag and release notes.
- Release notes include: upstream commit hash, build date, list of camera
  models, and known issues.
- If any camera build artifact is missing, the release title is prefixed with
  `[PARTIAL]` and the release body explicitly lists which camera binaries are
  absent. This prevents testers from discovering missing binaries after download.

---

### REQ-006 -- Continuous Improvement Loop

**User Story:** As a developer, I want a documented workflow for collecting new
logs, retraining LUTs, and redeploying to the camera, so that the AI model
improves over time with real shooting data.

**Acceptance Criteria:**

- `ARCHITECTURE.md` documents the full data flow:
  Camera logs -> Upload to Colab -> Train -> Export unified.tbl ->
  Copy to SD card -> Camera applies LUT -> New logs collected.
- Log files from SD card are used directly as input to
  `lut_training_multi_param.py` without preprocessing.
- Retrained `unified.tbl` replaces the SD card file; takes effect on the
  very next half-press (hot-swap, no reflash).
- Improvement loop is runnable without CI (manual Colab + SD card copy).

---

### REQ-007 -- Validation and Testing

**User Story:** As a developer, I want a documented validation process that
confirms LUT decisions are actually applied to CR2 RAW metadata.

**Acceptance Criteria:**

- `VALIDATION.md` documents step-by-step validation using `exiftool`.
- Metrics: ISO match rate (%), WB match rate (%),
  ETTR histogram success (after reduce_shutter, 90th-percentile bin in 180-240).
- `TROUBLESHOOTING.md` covers: LUT not loaded, wrong ISO/WB in CR2, CI failures.
- Debug logging to `A:/ML/logs/debug.txt` via verbose flag in Lua scripts.

---

### REQ-008 -- Repository Structure and Documentation

**Acceptance Criteria:**

- Directories: `lua_scripts/`, `colab/`, `models/`, `logs/`,
  `.github/workflows/`, `release_templates/`, `docs/`.
- `README.md`, `INSTALL.md`, `CONTRIBUTING.md`, `LICENSE` (MIT) present.

---

### REQ-009 -- GitHub Remote Connection

**Acceptance Criteria:**

- Remote: `git@github.com:fakhrur-hot/magiclantern_enhanced.git`.
- Working branch: `ai-lut-integration`. Never push to `main` or `master`.
- CI workflows trigger after push.

---

## Non-Functional Requirements

- **Platform:** EOS 6D DIGIC 5+ ARM Cortex-R4. No GPU. No FPU in Lua path.
  All on-camera logic is integer-only Lua. No floating-point permitted.
- **Performance:** Half-press routine < 100 ms total (LUT load + lookup + apply + log).
- **Memory:** All Lua scripts combined < 10 KB. LUT file <= 1 MB.
- **Safety:** All hardware calls use official Magic Lantern API only.
- **Portability:** LUT format and Lua scripts are camera-model-agnostic.
- **Maintainability:** Loader, decision, apply, and log functions are
  independently testable modules within each Lua script.

---

## Glossary

- **ARM Cortex-R4:** The CPU in the EOS 6D DIGIC 5+ processor. Real-time
  class, integer-optimized. Lua scripts run on this CPU with no GPU available.
- **Integer-only math:** All on-camera arithmetic uses whole numbers. No `/`
  division with non-integer results, no `math.floor`, no decimal constants.
- **LUT (Lookup Table):** `unified.tbl` -- a pipe-delimited text table mapping
  Scene + LightLevel to camera decisions. Read by Lua; never executed as a model.
- **ETTR (Expose To The Right):** Shutter adjustment to push histogram right
  without clipping. Encoded as integer shutter denominator pairs.
- **90th-percentile bin:** Integer histogram analysis: the bin index where
  cumulative pixel count reaches 90% of total. Signals near-highlight exposure.
- **WB multipliers:** Integer sensor gain ratios (G=100 reference). Applied
  directly to `set_wb()`. No Kelvin temperature conversion on camera.
- **ALO / HTP:** Canon in-camera tone processing features. Accessed via
  Magic Lantern API hooks; placeholder in current Lua until API is confirmed.
- **Decision Tree:** Scikit-learn model trained in Colab, predicts ISO from
  LightLevel. Runs only offline -- output is baked into `unified.tbl`.
- **Hot-swap:** Replacing `unified.tbl` on the SD card takes effect on the
  next half-press with no firmware reflash required.
