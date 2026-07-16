# Validation

How to confirm the AI-LUT decisions are actually applied to CR2 RAW metadata,
and how to measure how well they match real shooting conditions (REQ-007).

## Prerequisites

- [`exiftool`](https://exiftool.org/) installed.
- A test SD card with `unified_logger.lua` + `decision_engine.lua` deployed and
  a seed `unified.tbl` in `A:/ML/models/`.

## 1. Confirm decisions reached the CR2

After capturing shots with `decision_engine.lua` active, read the metadata:

```sh
exiftool IMG_XXXX.CR2 | grep -E "ISO|Shutter|White Balance|Red Balance|Blue Balance"
```

Compare each field against the decision the engine logged for that frame in
`A:/ML/logs/unified_log.txt` (the `Decision_*` lines).

## 2. Metrics

Capture a batch under known conditions (daylight, tungsten, shade, lowlight),
then compute:

| Metric | Definition | Target |
|---|---|---|
| **ISO match rate** | % of frames whose CR2 ISO equals `Decision_ISO` | 100% (exact) |
| **WB match rate** | % of frames whose CR2 WB gains match `Decision_WB` (R,G,B) | 100% (exact) |
| **ETTR histogram success** | after `reduce_shutter`, the 90th-percentile bin of the resulting frame falls in **180-240** | >= 90% of reduce_shutter frames |

ISO/WB are exact-match checks: the LUT stores the final integer values passed to
`set_iso()` / `set_wb()`, so a correctly applied decision reproduces them exactly
in the CR2. Any mismatch indicates an apply-path or Lua API problem (see
`TROUBLESHOOTING.md`).

### ETTR histogram check

`reduce_shutter` halves exposure to pull near-clipping highlights left. Validate
by re-reading the captured frame's histogram: the 90th-percentile bin (the same
integer statistic `compute_light_level` uses) should land in **180-240** -- bright
but no longer clipping. A value still > 240 means the single `reduce_shutter`
step was insufficient for that scene; a value < 180 means it over-corrected.

## 3. Fallback and error validation

- **Missing LUT:** remove `unified.tbl`; capture a frame; confirm fallback
  defaults (ISO 400, WB `R100,G100,B100`, `keep_shutter`) in the CR2 and a
  `FALLBACK_USED` (and `LUT_NOT_FOUND`) entry in the log.
- **Bad WB row:** feed a malformed WB string; confirm the CR2 shows neutral
  `set_wb(100,100,100)` and the log contains
  `WB_PARSE_ERROR|raw=...|fallback=R100,G100,B100`.
- **Nil histogram:** confirm a single `HIST_NIL|LightLevel=128(fallback)` entry
  rather than a silently mislabeled mid-tone record.

## 4. Record results

Log the ISO match rate (%), WB match rate (%), and ETTR histogram result for
each validation round here as the pipeline matures (populated during Task 10).
