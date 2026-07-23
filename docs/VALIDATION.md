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

## Known Limitations

### Dual-ISO MakerNote Patch

**Status:** UNSUPPORTED on EOS 6D (offset TBD).

The `ai_makernote_dualiso_patch()` function requires a verified byte offset into
the CR2 MakerNote IFD to write dual-ISO status. On the EOS 6D, this offset has
not been determined via hex dump (`exiftool -v3 -canon`). Until a valid offset is
confirmed, the `AI_MKNOTE_DUALISO_OFFSET_TODO` define remains `0` and the patch
is a no-op.

**Impact on RaZStudio consumers:** The `.ml6d` sidecar `dualIsoPatch` field will
read `"unsupported"` for all EOS 6D captures. RaZStudio MUST NOT attempt to
inspect MakerNote tags for dual-ISO detection on these files. Instead, use the
sidecar's `dualIso` block (present when dual-ISO is enabled and active for a
given shot) as the authoritative source for dual-ISO state.

**Resolution path:** Capture a dual-ISO CR2 on the EOS 6D, run
`exiftool -v3 -canon IMG_XXXX.CR2`, locate a suitable unused SHORT tag in the
MakerNote IFD, and assign the verified offset to the define. Once assigned, the
sidecar will emit `"dualIsoPatch":"applied"` and the MakerNote will carry the
dual-ISO flag directly in the CR2 EXIF.


---

## RaZStudio Integration Validation

End-to-end on-device validation proving the `.ml6d` sidecar and `ml_export.json`
pipeline works from shutter press through RaZStudio import (REQ-006).

This requires **hardware in the loop** (EOS 6D + SD card) and cannot be run in
CI. Use the same spare test card workflow as the AI-LUT Task 10 runbook.

Companion tools:
- `source-dev/build_tools/validate_sidecar.py` — batch-validates all sidecars on
  a pulled card against the contract schema.
- `docs/RAZSTUDIO_CONTRACT.md` — canonical field definitions and degradation rules.

---

### Prerequisites

- Firmware built from `source-dev/` on the `ai-lut-integration` branch with
  REQ-001 (call-site fix) and REQ-002 (dual-ISO offset/deferral) applied.
- `autoexec.bin` + matching `.sym` file deployed together on the EOS 6D test card
  (see `[[ml6d-deploy-matched-pair]]`).
- `exiftool` installed on the host PC.
- Python 3.8+ with no extra dependencies (for `validate_sidecar.py`).
- (Optional) RaZStudio installed on an Android device, or the contract test
  harness implementing REQ-005's import adapter interface.

---

### Test Sequence

1. **Build firmware**
   ```sh
   cd source-dev/
   git checkout ai-lut-integration
   make clean && make
   ```
   Confirm `autoexec.bin` and `autoexec.sym` are produced without errors.

2. **Flash to test card**
   - Copy `autoexec.bin` and `autoexec.sym` to the root of the EOS 6D SD card.
   - Ensure `ML/DATA/SHOTS/` directory exists (firmware creates it, but verify).
   - Ensure a valid `ML/models/lens_tune.tbl` is present if lens-tune fields are
     expected.

3. **Capture test shots**
   Minimum captures required:
   - **1× Non-ETTR shot** — ETTR module disabled in ML menu; any scene.
   - **1× ETTR shot** — ETTR enabled; meter a bright scene so ETTR fires.
   - **1× Dual-ISO shot** — Dual-ISO module enabled (if available on test body).
   - (Recommended) Additional shots across varying WB conditions (daylight,
     tungsten, shade) to stress-test WB multiplier diversity.

4. **Pull card to PC**
   - Mount or card-reader the SD card.
   - Note the card root path (e.g. `E:\` on Windows, `/media/user/EOS_DIGITAL/`
     on Linux).

5. **Run `validate_sidecar.py`**
   ```sh
   python source-dev/build_tools/validate_sidecar.py E:\
   ```
   Expected output: per-file pass/fail, summary table, and overall PASS/FAIL.
   Any failures indicate a firmware-side sidecar write issue.

6. **Manual sidecar inspection**
   Pick one sidecar per capture type and review the JSON manually:
   ```sh
   cat ML/DATA/SHOTS/IMG_XXXX.ml6d | python -m json.tool
   ```
   Confirm:
   - `schema_version` is `"2.0"`
   - `filename` matches the corresponding CR2
   - `ettr` block present only on ETTR shots
   - `dualIso` block present only on dual-ISO shots
   - `dualIsoPatch` field present on ALL shots (`"unsupported"` or `"applied"`)
   - `lens` and `pictureStyle` present on ALL shots

7. **Import into RaZStudio (or test harness)**
   - Open RaZStudio → Import → select the card/folder.
   - Confirm RaZStudio reads `ML/DATA/ml_export.json` (session metadata).
   - For each imported CR2, confirm WB corrections appear as the initial develop
     state (overridable, not baked).
   - Confirm lens-tune contrast/saturation/colorTone apply as picture-style
     starting point.
   - If using the contract test harness instead: run the harness against the card
     root and review its output for import success per CR2.

8. **Verify corrections applied without manual intervention**
   - WB multipliers in the develop module should match the sidecar's
     `lens.wbR` / `lens.wbB` values (if present).
   - Lens-tune values should match the session export's
     `lensTune.contrast` / `.saturation` / `.colorTone`.
   - All corrections are tagged "initial" (user-overridable), never locked.

---

### exiftool Reference Commands

**Inspect MakerNote tags (verbose Canon decode):**
```sh
exiftool -v3 -canon IMG_XXXX.CR2
```
Use this to verify dual-ISO MakerNote patch presence (when offset is resolved).
Look for the patched SHORT tag at the expected IFD offset.

**Read WB and ISO metadata from CR2:**
```sh
exiftool IMG_XXXX.CR2 | grep -E "ISO|White Balance|Red Balance|Blue Balance"
```
Compare against the sidecar's `lens.wbR` / `lens.wbB` and the session's ISO
state to confirm values were written to the CR2 EXIF correctly.

**Pretty-print sidecar content:**
```sh
cat ML/DATA/SHOTS/IMG_XXXX.ml6d | python -m json.tool
```

**Pretty-print session export:**
```sh
cat ML/DATA/ml_export.json | python -m json.tool
```

**Batch list all sidecars on a card:**
```sh
find /path/to/card/ML/DATA/SHOTS -name "*.ml6d" -exec echo {} \;
```

---

### Results Table

Record pass/fail for each capture type after running the validation sequence.

| Capture Type | Sidecar Present | JSON Valid | Fields Correct | RaZStudio Import | Notes |
|--------------|:---------------:|:----------:|:--------------:|:----------------:|-------|
| Non-ETTR     |                 |            |                |                  |       |
| ETTR         |                 |            |                |                  |       |
| Dual-ISO     |                 |            |                |                  |       |
| Session file |                 |            |                |                  |       |

**Column definitions:**
- **Sidecar Present** — `.ml6d` file exists in `ML/DATA/SHOTS/` for the CR2.
- **JSON Valid** — Sidecar parses as valid JSON (`python -m json.tool` exits 0).
- **Fields Correct** — Required fields present and values match shooting
  conditions (see checklist in step 6).
- **RaZStudio Import** — Corrections appear in the develop module without manual
  configuration.
- **Notes** — Any anomalies, Lua/C API mismatches, or firmware issues found.

---

### Expected Outcomes

| Capture Type | Expected Behavior |
|--------------|-------------------|
| **Non-ETTR** | Sidecar present with `lens`, `pictureStyle`, `dualIsoPatch` fields. No `ettr` block. WB and lens-tune corrections applied in RaZStudio. |
| **ETTR** | Sidecar present with all Non-ETTR fields PLUS `ettr` block (`lightLevel`, `sceneDR`, `highlightHeadroom`, `channelClip`). Only one sidecar per shot (no duplicates from old ETTR call site). |
| **Dual-ISO** | Sidecar present with `dualIso` block (`enabled:true`, `isoBase`, `isoAlternate`, `interleavePeriod`). `dualIsoPatch` reads `"unsupported"` (EOS 6D) or `"applied"` (if offset resolved). RaZStudio uses sidecar block for detection. |
| **Session file** | `ML/DATA/ml_export.json` present, valid JSON, contains `schema_version:"2.0"`, `firmwareVersion`, `body`, `sensor`, `session` blocks. Read once by RaZStudio at import start. |

---

### Failure Triage

If any row in the results table fails:

| Symptom | Likely Cause | Action |
|---------|-------------|--------|
| Sidecar missing for non-ETTR shot | `CBR_POST_SHOOT` not firing or card write failed | Check `MODULE_CBRS` registration; verify card has free space |
| Sidecar has double-extension (`.CR2.ml6d`) | Basename not stripped before `ai_sidecar_write()` | Verify `ai_post_capture_cbr` passes `"IMG_NNNN"` not `"IMG_NNNN.CR2"` |
| `ettr` block present on non-ETTR shot | Stale state from prior ETTR capture | Verify `auto_ettr` guard in `ai_sidecar_write()` |
| Duplicate sidecars per ETTR shot | Old call site at `ettr.c:986` not removed | Confirm task 1.3 dedup fix is applied |
| `schema_version` missing | `snprintf` format string not updated | Verify tasks 3.1/3.2 |
| RaZStudio doesn't apply corrections | Import adapter not reading `.ml6d` path | Check contract path matches `ML/DATA/SHOTS/` |
| `ml_export.json` missing | `ai_export_session_write()` not called at init | Check `ettr_init()` call at module load |
