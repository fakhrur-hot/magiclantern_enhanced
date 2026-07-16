# Field Logging Checklist

For collecting clean training data with `unified_logger.lua` on the EOS 6D.
Logs are written on each **half-press** to `A:/ML/logs/unified_log.txt`.
Companion: `docs/TASK10_RUNBOOK.md`, `tools/analyze_log.py`.

## Why half-press
At half-press the camera has metered the scene: the histogram (→ integer
90th-percentile LightLevel) and current ISO/shutter are available, and no photo
is taken yet — so logging never interferes with capture.

---

## Before you shoot (setup)

- [ ] Spare SD card (never the primary) with Magic Lantern installed.
- [ ] `ML/scripts/unified_logger.lua` present; `ML/logs/` folder exists.
- [ ] Magic Lantern **Lua module enabled**.
- [ ] **Ground-truth pass: turn the AI OFF** — Expo → Auto ETTR → **Auto ISO
      Optimizer = OFF**. This makes the log capture the camera's *native*
      metering, not values the AI already changed (avoids feedback bias).
- [ ] **Be in LiveView** while half-pressing — on the 6D the histogram is only
      populated in LV. OVF half-press → `HIST_NIL` / useless `LightLevel=128`.
- [ ] Shoot RAW (CR2) if you also want to validate results later with exiftool.

## Per session = one lighting type

Scene is **not** auto-tagged on camera yet (`Scene=unknown` in every row), so
capture one lighting type per session and **write down which session is which**;
you relabel the `Scene` field offline before training.

Cover all four LUT scenes across sessions:
- [ ] **daylight** (open sun ~5500K)
- [ ] **shade** (open shade / overcast ~6500K)
- [ ] **tungsten** (indoor incandescent ~3200K)
- [ ] **lowlight** (dim mixed sources)

## During a session

- [ ] Half-press across a **range of brightness** within the scene (bright
      highlights → deep shadow) so `LightLevel` spans its range, not one value.
- [ ] Aim for **plenty of samples** — the trainer flags below 200
      (`low_sample_warning`); more per scene = better ISO/threshold fit.
- [ ] Vary framing/subject a little; keep the lighting type constant.
- [ ] Note start/end frame or time so you can map the session → scene later.

---

## After shooting (before training)

- [ ] Copy `A:/ML/logs/unified_log.txt` to your computer (back it up per session).
- [ ] **Relabel scenes**: set the `Scene=` line per session block to the real
      scene (daylight/shade/tungsten/lowlight). Until on-camera scene detection
      exists, this is what makes per-scene WB learnable.
- [ ] **Audit**: `python tools/analyze_log.py unified_log.txt`
      - LightLevel all integers in 0–255, tracking visual brightness.
      - No unexpected `HIST_NIL` (means you weren't in LiveView).
      - Check record count per scene.

## Train + deploy

- [ ] Run `colab/lut_training.ipynb` (or `lut_training_multi_param.py`) → new
      `unified.tbl`. Review the header (`training_samples`, `low_sample_warning`,
      `wb_fallback_scenes`) and `training_audit.log`.
- [ ] Copy `unified.tbl` to `A:/ML/models/` (hot-swap; no reflash).
- [ ] **Turn the AI back ON** (Auto ISO Optimizer) to shoot with the learned LUT.
- [ ] Validate applied results with exiftool per `docs/VALIDATION.md`.

## Repeat

Each new session adds samples; retrain to refine ISO predictions and (once real
scene labels accumulate) per-scene WB. Keep ground-truth passes AI-off; keep
shooting passes AI-on.

---

### Quick reference

| Do | Don't |
|----|-------|
| Half-press in **LiveView** | Log through the optical finder |
| **One lighting type per session** | Mix scenes in one session |
| Vary brightness within a scene | Shoot only one exposure |
| **AI OFF** for ground-truth logging | Log while the AI is changing settings |
| Audit + relabel before training | Train on raw `Scene=unknown` logs |
