# Architecture

Full data flow for the AI-assisted exposure/color pipeline (REQ-006). The
authoritative component-level design lives in
[.kiro/specs/ai-lut-magic-lantern/design.md](.kiro/specs/ai-lut-magic-lantern/design.md);
this document is the operational overview.

## Overview

Four layers connected by a continuous improvement loop:

1. **Camera layer (EOS 6D)** -- integer-only Lua on the DIGIC 5+ ARM Cortex-R4
   (no FPU, no GPU): `unified_logger.lua` (study mode) and `decision_engine.lua`
   (applies decisions). All on-camera work is file read, table lookup, and
   integer arithmetic.
2. **Offline training layer (Google Colab)** -- Pandas + scikit-learn
   (`DecisionTreeClassifier`) turn logs into `unified.tbl`.
3. **CI/CD layer (GitHub Actions)** -- `build.yml` builds firmware for the
   camera matrix; `release.yml` publishes per-camera releases with the LUT.
4. **Continuous improvement loop** -- new logs feed the next retraining.

## Data Flow

```
+-- CAMERA (EOS 6D) -------------------------------------------------+
|  half-press --> unified_logger.lua --> A:/ML/logs/unified_log.txt  |
|  half-press --> decision_engine.lua                                |
|      load unified.tbl -> compute_light_level -> find_decision      |
|      -> set_shutter_fraction / set_iso / set_wb (+ alo/htp stubs)  |
|      -> log_decision                                               |
+-------------------------------------------------------------------+
        |  (export unified_log.txt from SD card)
        v
+-- OFFLINE TRAINING (Colab / lut_training_multi_param.py) ----------+
|  parse log -> DataFrame -> DecisionTreeClassifier(max_depth=4)     |
|  + integer ETTR/ALO/HTP thresholds + scene WB table                |
|  -> deterministic, sorted unified.tbl (+ iso_model.joblib)         |
+-------------------------------------------------------------------+
        |  (copy unified.tbl back to A:/ML/models/  +  git push)
        v
+-- CI/CD (GitHub Actions) -----------------------------------------+
|  build.yml (matrix) -> firmware + unified-lut artifacts           |
|  release.yml -> release/EOS-<model>/ + [PARTIAL] handling         |
+-------------------------------------------------------------------+
        |
        v
   decision_engine reads the updated LUT on the next half-press
   -> new logs collected -> cycle repeats
```

## The improvement loop (no CI required)

The loop is runnable entirely by hand: export `unified_log.txt`, run
`lut_training_multi_param.py` (or the notebook), copy the resulting
`unified.tbl` to `A:/ML/models/`. The new LUT is **hot-swapped** -- it takes
effect on the very next half-press because `load_unified_model` runs fresh each
time; no reflash, no reboot. CI (build/release) is an optional convenience for
distributing firmware, not part of the retraining path.

## Key invariants

- **Integer-only on camera.** No floats, no fractional division; `math.floor`
  only in `compute_light_level`.
- **Threshold alignment.** The training thresholds (200/30/20/50) are the same
  integers the LUT encodes; the camera never re-evaluates them.
- **Deterministic training.** Same log in -> byte-identical `unified.tbl` out
  (fixed seed, sorted rows); notebook and script agree.
