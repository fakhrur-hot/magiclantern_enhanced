# Implementation Plan: ALO/HTP Scene Optimizer

## Overview

Replace the float-based `ai_apply_alo_htp(float scene_dr, float highlight_headroom)` in `ettr.c` with an integer-only, parameterless version that reads P90/P99 from existing file-scope statics and classifies scenes into Dark/Normal/Bright zones. Update both call sites (M-mode LiveView path and OVF review path) to use the new void signature.

## Tasks

- [ ] 1. Replace ai_apply_alo_htp function body and update call sites
  - [ ] 1.1 Replace the old float-based ai_apply_alo_htp with the new integer-only implementation
    - Remove the old `#define` constants (`AI_HTP_HEADROOM_EV`, `AI_ALO_LOW_DR_EV`, `AI_ALO_STD_DR_EV`, `AI_ALO_HIGH_DR_EV`) at ~line 262–265
    - Replace the function signature `static void ai_apply_alo_htp(float scene_dr, float highlight_headroom)` with `static void ai_apply_alo_htp(void)`
    - Implement the new body: call `ai_light_level()` for P90, read `ai_raw_p99` for P99, classify zone (Bright if P99≥240, Dark if P90<80, else Normal), compute `want_htp`/`want_alo` per the design decision table, handle ISO100+HTP conflict, apply setters with HTP-first ordering and skip-if-unchanged logic
    - Add the new `#define` thresholds: `AI_DARK_SLIGHT 80`, `AI_DARK_MODERATE 50`, `AI_DARK_VERY 25`, `AI_BRIGHT_P99 240`
    - _Requirements: 1.1, 3.1, 3.2, 3.3, 4.1–4.5, 5.1–5.3, 6.1–6.3, 7.1–7.3, 9.1–9.3, 10.1, 10.2_

  - [ ] 1.2 Update the M-mode call site in auto_iso_optimizer_step (~line 2150–2158)
    - Remove the `ettr_metadata_t md = ettr_last_metadata()` fetch and the sentinel check `if (!(md.light_level == 0 && ...))`
    - Replace `ai_apply_alo_htp(md.scene_dr, md.highlight_headroom)` with `ai_apply_alo_htp()`
    - The new function handles the "no data" case internally (returns early when `ai_light_level()` returns -1)
    - _Requirements: 2.1, 8.1, 8.2_

  - [ ] 1.3 Update the OVF review call site (~line 1978–1988)
    - Remove the local `highlight_headroom` / `scene_dr` float computations and the `ai_apply_alo_htp(scene_dr, highlight_headroom)` call
    - Replace with a plain `ai_apply_alo_htp()` call (still inside the `if (meter_ok && auto_iso_optimizer)` gate)
    - Remove the `int pcts[3] = {999, 950, 50}` third element (the `50` / 5th-percentile shadow entry) if it was only used for `scene_dr` here — or leave it if other code below uses `lvls[2]`
    - _Requirements: 2.1, 2.3, 3.1, 3.2_

- [ ] 2. Build verification and deploy
  - [ ] 2.1 Compile the firmware and verify zero errors/warnings on the modified function
    - Run: `wsl bash -c "cd /mnt/c/Users/Public/Kiro/ML_6D/source-dev && make -C platform/6D.116 -j4"`
    - Confirm `ettr.mo` and `autoexec.bin` build successfully
    - Fix any compiler errors or warnings
    - _Requirements: 9.1, 9.2, 9.3_

  - [ ] 2.2 Deploy build output to SD card
    - Copy `source-dev/platform/6D.116/build/autoexec.bin` to `D:\autoexec.bin`
    - Copy `source-dev/platform/6D.116/build/6D_116.sym` to `D:\ML\modules\6D_116.sym`
    - Copy `source-dev/platform/6D.116/build/modules/ettr.mo` to `D:\ML\modules\ettr.mo`
    - _Requirements: 2.2_

- [ ] 3. Final checkpoint
  - Ensure the build completes cleanly with no warnings, ask the user if questions arise.

## Notes

- This is a surgical replacement: only the function body at ~line 267 and two call sites (~line 1987, ~line 2157) change
- The `ai_light_level()` call inside the new function body is safe to call multiple times per poll cycle (it re-reads the same histogram buffer)
- The OVF review path's `pcts[]/lvls[]` arrays may still be needed by code below (EC correction logic) — only remove the `scene_dr`/`highlight_headroom` float computation, not the raw percentile read itself
- No new files created; no new includes needed (`ai_lut.h` is already included and defines `ai_light_level()`, `ai_raw_p99`)
- Property tests are not applicable here (bare-metal C firmware with no test harness on-target)

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1"] },
    { "id": 1, "tasks": ["1.2", "1.3"] },
    { "id": 2, "tasks": ["2.1"] },
    { "id": 3, "tasks": ["2.2"] }
  ]
}
```
