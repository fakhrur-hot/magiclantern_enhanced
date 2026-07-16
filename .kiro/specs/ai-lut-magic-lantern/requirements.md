# Requirements Document

## Introduction

This project builds an end-to-end AI-assisted exposure and color control pipeline for Canon DSLRs running Magic Lantern. It combines on-camera Lua scripting, offline model training in Google Colab, a unified LUT format, and CI/CD automation to deliver optimized ETTR, ALO, HTP, ISO, and WB decisions before each CR2 RAW capture.

The system is designed around the constraints of the EOS 6D's DIGIC 5+ ARM Cortex-R4 processor: no on-camera ML inference, integer-only math, small memory footprint, and SD card I/O for model storage.

The GitHub remote repository is `fakhrur-hot/magiclantern_enhanced`. All AI feature work lives on the `ai-lut-integration` branch.

---

## Requirements

### REQ-001 — On-Camera Sensor Data Logging

**User Story:** As a photographer using Magic Lantern, I want the camera to automatically log raw sensor data during every half-press shutter event, so that I can collect a dataset for offline AI model training.

**Acceptance Criteria:**

- The Lua logging script hooks into the Magic Lantern half-press shutter event.
- Each log entry captures: timestamp, histogram bins, scene mode, light level, current ISO, current WB, and current shutter speed.
- Entries are appended to `A:/ML/logs/unified_log.txt` on the SD card in a key=value format with `---` as entry separator.
- The script runs without modifying any camera settings (study/logging mode only).
- Script size remains under 10 KB.
- Logging completes within 100 ms to maintain responsiveness.

---

### REQ-002 — Unified LUT File Format

**User Story:** As a developer, I want a single compact LUT file that encodes all AI decisions (ETTR, ALO, HTP, ISO, WB) per scene and light level, so that the on-camera Lua script can look up all parameters in one read.

**Acceptance Criteria:**

- LUT file (`unified.tbl`) uses pipe-delimited rows: `Scene|LightLevel|ETTR|ALO|HTP|ISO|WB`.
- Lines starting with `#` are treated as comments.
- ETTR values are: `reduce_shutter`, `keep_shutter`, `increase_shutter`.
- ALO values are: `shadow_lift`, `shadow_boost`, `neutral`.
- HTP values are: `priority_on`, `priority_off`.
- ISO is a numeric string (e.g., `100`, `400`, `1600`, `3200`).
- WB is in RGB multiplier format: `R120,G100,B90`.
- File size must remain ≤ 1 MB for fast SD card reads.
- File encoding is UTF-8 with no BOM.

---

### REQ-003 — On-Camera LUT Decision Engine

**User Story:** As a photographer, I want Magic Lantern to automatically load the LUT and apply optimized ETTR, ISO, and WB settings before each shot, so that my CR2 RAW files are consistently well-exposed.

**Acceptance Criteria:**

- The decision engine Lua script loads `unified.tbl` from `A:/ML/models/` on each half-press event.
- It looks up the current scene + light level key and retrieves ETTR, ALO, HTP, ISO, WB decisions.
- If no exact match exists in the LUT, safe fallback defaults are applied: `keep_shutter`, `neutral`, `priority_off`, ISO 400, WB `R100,G100,B100`.
- ETTR decision translates to a `set_shutter_fraction()` call.
- ISO decision translates to a `set_iso()` call.
- WB decision translates to a `set_wb()` call.
- ALO and HTP have placeholder implementations ready for future tone curve and highlight priority hooks.
- Every decision (applied or fallback) is logged to `A:/ML/logs/unified_log.txt`.
- All math uses integer operations only (no floating-point).
- Script size remains under 10 KB.

---

### REQ-004 — Offline LUT Training Pipeline (Google Colab)

**User Story:** As a developer, I want a Colab notebook that ingests camera logs, trains a lightweight model, and exports a ready-to-deploy `unified.tbl`, so that LUTs improve automatically as more log data is collected.

**Acceptance Criteria:**

- Colab notebook parses `unified_log.txt` into a Pandas DataFrame.
- Feature engineering converts LightLevel to integer and Scene to string categories.
- A scikit-learn DecisionTreeClassifier (max_depth=4) is trained for ISO prediction.
- LUT rows are generated for every unique Scene + LightLevel combination in the logs.
- ETTR, ALO, and HTP rules are derived from light level thresholds.
- WB multipliers are derived from scene category.
- Exported `unified.tbl` matches the pipe-delimited format defined in REQ-002.
- A standalone Python script (`lut_training_multi_param.py`) replicates the notebook for automated runs.
- Both notebook and script produce identical `unified.tbl` output given the same input logs.

---

### REQ-005 — CI/CD Build Pipeline (GitHub Actions)

**User Story:** As a developer, I want automated GitHub Actions workflows that build Magic Lantern firmware for multiple Canon camera models and publish releases with firmware binaries and the latest LUT, so that deployments are consistent and repeatable.

**Acceptance Criteria:**

- `build.yml` workflow triggers on push and PR to `ai-lut-integration` branch, and on a nightly schedule (02:00 UTC).
- Build matrix covers at minimum: `6D.116`, `5D3.113`, `60D.111`, `650D.104`.
- Each camera build runs `make clean && make` in `platform/<camera>/`.
- Compiled `magiclantern.bin` is uploaded as a named artifact per camera.
- `unified.tbl` is uploaded as a separate artifact.
- `release.yml` workflow triggers on completion of a successful `build.yml` run.
- Release job organizes artifacts into per-camera folders and creates a GitHub Release with a version tag and release notes.
- Release notes include: upstream commit hash, build date, list of camera models, and known issues.

---

### REQ-006 — Continuous Improvement Loop

**User Story:** As a developer, I want a documented workflow for collecting new logs, retraining LUTs, and redeploying to the camera, so that the AI model improves over time with real shooting data.

**Acceptance Criteria:**

- `ARCHITECTURE.md` documents the full data flow: Camera logs → Upload to Colab → Train models → Export unified.tbl → Copy to SD card → Camera applies LUT → New logs.
- Log files from SD card can be used directly as input to `lut_training_multi_param.py` without preprocessing.
- Retrained `unified.tbl` can replace the existing file on SD card without requiring a firmware reflash.
- The improvement loop is executable without CI (manual Colab run + SD card copy).

---

### REQ-007 — Validation and Testing

**User Story:** As a developer, I want a documented validation process that confirms LUT decisions are actually applied to CR2 RAW metadata, so that I can trust the system is working correctly.

**Acceptance Criteria:**

- `VALIDATION.md` documents step-by-step validation using `exiftool` to compare CR2 metadata against log entries.
- Defined metrics: ISO match rate (%), WB match rate (%), ETTR histogram success (highlights near but not clipping).
- `TROUBLESHOOTING.md` covers: LUT not loaded, wrong ISO/WB in CR2, build failures in CI.
- Debug logging to `A:/ML/logs/debug.txt` is available via a verbose flag in Lua scripts.

---

### REQ-008 — Repository Structure and Documentation

**User Story:** As an open-source contributor, I want a well-organized repository with clear documentation, so that I can understand, install, and contribute to the project without ambiguity.

**Acceptance Criteria:**

- Repository follows the defined directory structure: `lua_scripts/`, `colab/`, `models/`, `logs/`, `.github/workflows/`, `release_templates/`, `docs/`.
- `README.md` includes project summary, component list, and quick-start steps.
- `INSTALL.md` provides prerequisites and step-by-step SD card deployment instructions.
- `CONTRIBUTING.md` covers fork, branch, PR, code style, and bug reporting conventions.
- A `LICENSE` file (MIT) is present at repo root.

---

### REQ-009 — GitHub Remote Connection

**User Story:** As a developer working in this IDE, I want to connect the local repository to the GitHub remote `fakhrur-hot/magiclantern_enhanced`, so that I can push commits, trigger CI workflows, and create pull requests.

**Acceptance Criteria:**

- Remote can be connected via: IDE GitHub login (OAuth), Personal Access Token (scopes: `repo`, `workflow`), or SSH key (`ed25519`).
- SSH clone URL: `git@github.com:fakhrur-hot/magiclantern_enhanced.git`.
- The AI-feature branch is named `ai-lut-integration`.
- All AI-related commits are pushed to `ai-lut-integration`, never directly to `main` or `master`.
- CI workflows trigger correctly after push.

---

## Non-Functional Requirements

- **Performance:** Lua half-press routine completes in < 100 ms on EOS 6D DIGIC 5+ ARM Cortex-R4.
- **Memory:** All Lua scripts combined < 10 KB. LUT file ≤ 1 MB.
- **Safety:** All hardware interaction uses official Magic Lantern API calls only.
- **Portability:** LUT format and Lua scripts are camera-model-agnostic where possible.
- **Maintainability:** Lua scripts are modular — loader, decision, apply, and log functions are independently testable.
- **Compatibility:** The project forks Magic Lantern and stays syncable with upstream via standard git merge/rebase.

---

## Glossary

- **ML (Magic Lantern):** Open-source firmware add-on for Canon DSLRs.
- **Lua:** Lightweight scripting language used by Magic Lantern for automation.
- **LUT (Lookup Table):** A table mapping input values to output decisions. Stored as `unified.tbl`.
- **ETTR (Expose To The Right):** Exposure technique pushing histogram right without clipping highlights.
- **ALO (Auto Lighting Optimizer):** Canon feature that brightens shadows; represented as tone curve keywords.
- **HTP (Highlight Tone Priority):** Canon feature protecting highlights; toggled `priority_on/off`.
- **ISO:** Sensor sensitivity setting. Higher = brighter but noisier.
- **WB (White Balance):** Color temperature adjustment stored as RGB multipliers.
- **CR2 RAW:** Canon's RAW image format storing sensor data and metadata.
- **CI/CD:** Automated build and deployment pipelines via GitHub Actions.
- **Upstream:** The original Magic Lantern source repository.
- **Fork:** A copy of the upstream repo with AI features added.
- **Colab:** Google Colaboratory, the cloud Python environment used for offline training.
- **Decision Tree:** Scikit-learn classifier used to predict ISO from light level.
- **Artifact:** A file produced by CI (e.g., `magiclantern.bin`, `unified.tbl`).
