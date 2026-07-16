# ETTR + AI-LUT Firmware Integration

How the AI-LUT **improves over** the nightly ETTR instead of replacing or
fighting it. This is the "firmware integration (deepest)" path chosen over the
standalone Lua `decision_engine.lua`.

## Principle

**The real, metered ETTR keeps ownership of the exposure push.** The AI layer
only supplies the *learned per-scene starting points* that ETTR does not know
about: ISO, white balance, ALO and HTP. So you get ML's precise raw-histogram
ETTR **plus** priors learned from your own shooting — a strict superset of what
nightly ETTR does alone.

This is the opposite of the standalone Lua engine, which bypassed ETTR with a
crude fixed `set_shutter_fraction(1, 50/100/200)` and would overwrite ETTR's
decisions on every half-press.

## Where it hooks

`modules/ettr/ettr.c` already has the **Auto ISO Optimizer** (`auto_iso_optimizer_step`,
[[ml6d-ettr-htp-alo-modes]]): edge-triggered when Canon Auto ISO is engaged in
P/Av/Tv/M, it enables HTP+ALO and (in M) arms ETTR at an ISO floor.

The integration (`modules/ettr/ai_lut.h`) replaces the **hardcoded**
`set_htp(1)` / `set_alo(ALO_STD)` / `MIN_ISO` with **learned** values read from
`ML/models/unified.tbl`:

1. `ai_lut_load()` — `read_file("ML/models/unified.tbl", ...)` (< 8 KB, single
   FIO read), manual pipe/comma parse into rows (no reliance on `sscanf`
   scansets). Reloaded on each engage, so LUT hot-swap works.
2. `ai_light_level()` — integer 90th-percentile bin of the **green** channel of
   the ML `histogram` (REQ-001 definition), scaled 0..255. 128 fallback if the
   histogram is empty.
3. `ai_scene()` — best-effort scene from the camera's WB **Kelvin**
   (`lens_info.kelvin` when `wb_mode == WB_KELVIN`), else `"unknown"`.
4. `ai_find()` — exact match, then integer nearest-neighbor within scene, then
   within `"unknown"`.
5. `ai_lut_apply()` — applies `set_htp` / `set_alo` (and, only when the scene is
   confidently known, `lens_set_custom_wb_gains`) and returns the learned ISO.
   The optimizer uses that as ETTR's starting ISO (`ai_iso_to_raw`) instead of
   `MIN_ISO`.

If `unified.tbl` is missing or no row matches, the optimizer falls back to its
previous hardcoded behavior — nothing regresses.

## Deliberate design decisions (review these)

- **Metered ETTR owns exposure.** The AI never sets shutter directly; it sets
  the *starting* ISO + HTP/ALO/WB and lets ETTR meter from there.
- **WB only when scene is known.** WB gains override user WB (custom WB mode),
  so they are applied only when Kelvin gives a confident scene — otherwise the
  user's WB is left alone rather than forced to a guess.
- **Scene detection is best-effort (open item).** Kelvin-based scene only works
  in Kelvin WB; in Auto WB scene stays `"unknown"` and per-scene WB is skipped.
  A better scene signal is the main future improvement (tracked in
  [[ml6d-ai-lut-open-issues]] #4).
- **Light-level source must match the logger.** Inference uses the green-channel
  90th percentile of the 128-bin ML histogram; the training LightLevel comes
  from the logger. These must be reconciled on hardware (Task 10) so lookups are
  meaningful.

## Status

Compiles in the WSL toolchain. **NOT hardware-tested** — property writes,
histogram availability at engage time, and the Kelvin→scene mapping all need
on-camera verification (Task 10). Gated behind the existing "Auto ISO Optimizer"
menu toggle (default ON); disable it to get stock ETTR back.
