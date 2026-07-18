# Architecture

Data flow for the AI-assisted exposure/color pipeline on Magic Lantern (EOS 6D,
DIGIC 5+ ARM Cortex-R4 — no FPU, no GPU, integer-only on camera).

## Overview

Three layers connected by a continuous improvement loop. **All on-camera
intelligence lives in one place — the firmware ETTR module (`ettr.mo`)** — so
there is a single source of adjustment per feature, driven through Magic
Lantern's own controls and surfaced in its menu.

1. **Camera layer (firmware, `modules/ettr/`)** — integer-only C in the ETTR
   module:
   - **Data logging** — on each half-press, appends sensor stats to
     `A:/ML/logs/unified_log.txt` (green-channel light level from the RAW
     histogram, per-channel raw percentiles, ISO/shutter, WB, file number).
   - **Decision engine** — reads `A:/ML/models/unified.tbl` and applies the
     learned ISO / ALO / HTP through ML's own setters; the *metered* ETTR owns
     the exposure push. Nearest-neighbour lookup, integer-only.
   - **AI White Balance** — confidence-clipped bright-pixels auto-WB (RAW+JPEG).
   - **AI Picture Tune** — per-lens Canon contrast/saturation (JPEG).
   - **AI Modes** — restricts all of the above to the chosen shooting modes.
2. **Offline training (internal)** — turns exported logs into `unified.tbl`.
   Treated as a black box here; the on-camera side only consumes the LUT.
3. **CI/CD (GitHub Actions)** — `build.yml` builds firmware for the camera
   matrix; `release.yml` publishes per-camera releases with the LUT.

## Data flow

```
+-- CAMERA (EOS 6D, ettr.mo) ---------------------------------------+
|  half-press:                                                      |
|    log sensor stats  -> A:/ML/logs/unified_log.txt                |
|    read unified.tbl  -> integer nearest-neighbour lookup          |
|      -> set_iso floor / set_htp / set_alo   (ML setters)          |
|      -> metered ETTR owns the exposure push (M mode)              |
|      -> AI white balance (highlights)  -> custom WB gains         |
|      -> AI picture tune (per lens)     -> picstyle contrast/sat   |
+-------------------------------------------------------------------+
        |  (export unified_log.txt from the SD card)
        v
+-- OFFLINE TRAINING (internal) ------------------------------------+
|  logs -> model -> deterministic, sorted unified.tbl               |
+-------------------------------------------------------------------+
        |  (copy unified.tbl to A:/ML/models/)
        v
+-- CI/CD (GitHub Actions) -----------------------------------------+
|  build.yml (matrix) -> firmware + full installer artifacts        |
|  release.yml -> release/EOS-<model>/ + [PARTIAL] handling         |
+-------------------------------------------------------------------+
        |
        v
   the firmware reads the updated LUT on the next half-press
   -> new logs collected -> cycle repeats
```

## The improvement loop (no CI required)

Runnable entirely by hand: export `unified_log.txt`, produce a new
`unified.tbl` offline, copy it to `A:/ML/models/`. The LUT is **hot-swapped** —
it takes effect on the very next half-press (loaded fresh each time; no reflash,
no reboot). CI is an optional convenience for distributing firmware, not part of
the retraining path.

## Key invariants

- **Single source of adjustment.** Each of ETTR/ISO, WB, ALO, HTP is driven from
  exactly one place (the ETTR module), through ML's own controls and menu.
- **Integer-only on camera.** No floats, no fractional division; the DIGIC 5+
  ARM Cortex-R4 has no FPU.
- **RAW-based light level.** Exposure is read from the RAW histogram (true
  sensor data), not the ExpSim-brightened preview.
- **LUT fits the camera.** The LUT is capped (≤128 rows, <8 KB single read);
  training quantizes to stay within it.
