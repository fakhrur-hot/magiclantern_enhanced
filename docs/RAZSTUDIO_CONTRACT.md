# RaZStudio Integration Contract

> ## ⚠️ RETIRED 2026-07-26 — this contract is NOT in use
>
> **StudioRoom does not consume `.ml6d` sidecars or `ml_export.json`.** Its own
> source has the sidecar consumer commented out:
> `// MLExtendedIntelligence disabled — sidecar-driven corrections caused issues.`
> The confirmed-working path reads **standard Canon EXIF only** (via LibRaw):
> **LensID → per-lens tune, ISO → noise reduction, ColorTemperature → WB/tint**.
> No sidecar, no session file, no MakerNote dual-ISO tag are read.
>
> Consequently the firmware writers (`ai_sidecar_write`, `ai_export_session_write`,
> `ai_makernote_dualiso_patch`) were **removed from `ettr.c`/`ai_lut.h`**
> 2026-07-26. They had no consumer and were failing to write to `ML/DATA`
> anyway. The firmware's on-camera WB/exposure/lens adjustments already land in
> the standard CR2 EXIF StudioRoom reads, so nothing extra is needed.
>
> This document is kept for historical reference only. The accurate,
> shipped integration is StudioRoom's `.kiro/specs/ml6d-cr2-integration`
> (EXIF-only). One open verification: confirm ML's custom WB gains actually
> change the CR2's LibRaw-read ColorTemperature/`cam_mul` (field EXIF showed
> ColorTemperature stuck at 4100 while gains varied) — if not, on-camera AI WB
> does not reach StudioRoom and only the `lens_tune.tbl` WB trim applies.

---

Version: 2.0 (RETIRED)  
Last updated: 2026-07-26 (retired — StudioRoom uses EXIF-only, sidecars removed)  
Spec: `RaZStudio_integration`

This document defines the on-disk contract between the ML_6D firmware
(EOS 6D Magic Lantern fork) and RaZStudio (companion Android RAW
processor). The firmware writes these files to the SD card at capture
time; RaZStudio reads them at import time.

**The file format IS the API.** No shared code, no repo coupling.
Schema versioning handles firmware/consumer drift.

---

## Table of Contents

1. [Schema Version Semantics](#schema-version-semantics)
2. [`.ml6d` Sidecar Schema](#ml6d-sidecar-schema)
3. [`ml_export.json` Session Schema](#ml_exportjson-session-schema)
4. [Degradation Rules](#degradation-rules)
5. [Examples](#examples)
6. [Import Adapter Interface](#import-adapter-interface)

---

## Schema Version Semantics

Both contract files carry a `schema_version` field formatted as a
`"<major>.<minor>"` string (e.g. `"2.0"`).

| Version change | Meaning | Consumer behavior |
|----------------|---------|-------------------|
| Minor bump (e.g. `"2.0"` → `"2.1"`) | Additive fields only. No removals or renames. | Consumer SHOULD accept gracefully. Unknown fields are ignored. |
| Major bump (e.g. `"2.0"` → `"3.0"`) | Breaking change: field removal, rename, or semantic change. | Consumer MUST reject the file and fall back to default processing. |

**Rejection rule:** If the file's major version is greater than the
consumer's known maximum, treat the file as missing (see Degradation
Rules below).

The legacy `formatVersion` integer field (`2`) is retained for backward
compatibility with early test builds. `schema_version` is the canonical
version consumers MUST check.

---

## `.ml6d` Sidecar Schema

**Writer:** `ai_sidecar_write()` in `source-dev/modules/ettr/ai_lut.h`  
**Trigger:** `file_number`-change poll inside `auto_ettr_polling_cbr` (`CBR_SHOOT_TASK`) —
fires once per new capture, all shooting modes. **Not** a `CBR_POST_SHOOT` handler: that
mechanism was tried first and confirmed dead on this fork 2026-07-21 (grepping every
`module_exec_cbr()` call site in `source-dev/src` shows `CBR_PRE_SHOOT`/`CBR_POST_SHOOT` are
never invoked anywhere in this fork's core, so a registered handler for it can never run — a
real on-camera regression caught by shooting 4 test CR2s and finding zero sidecars). The polling
approach is proven to work because `CBR_SHOOT_TASK` already drives other logic in this same file.  
**Path:** `ML/DATA/SHOTS/{CR2_BASENAME}.ml6d`  
**Encoding:** UTF-8 text, valid JSON  
**One file per CR2 capture.**

### Field Reference

| Field | Type | Present when | Description |
|-------|------|-------------|-------------|
| `formatVersion` | integer | Always | Schema format generation. Currently `2`. |
| `schema_version` | string | Always | Semantic version `"<major>.<minor>"`. Currently `"2.0"`. |
| `filename` | string | Always | Full CR2 filename including extension (e.g. `"IMG_5290.CR2"`). |
| `lens` | object | Always | Lens tuning data from `lens_tune.tbl`. |
| `lens.id` | integer | Always | Canon lens ID (from `lens_info.lens_id`). `0` = default/unknown. |
| `lens.caStrength` | integer | Always | Chromatic aberration correction strength, range `[0..100]`. |
| `lens.fringeReduce` | integer | Always | Purple/green fringe suppression strength, range `[0..100]`. |
| `pictureStyle` | string | Always | Active Canon Picture Style name (e.g. `"STANDARD"`, `"NEUTRAL"`, `"PORTRAIT"`). |
| `dualIso` | object | Conditional | Only present when dual-ISO is enabled AND active for this shot. |
| `dualIso.enabled` | boolean | When `dualIso` present | Always `true` (block is omitted entirely when not active). |
| `dualIso.isoBase` | integer | When `dualIso` present | Lower ISO value of the dual-ISO pair. |
| `dualIso.isoAlternate` | integer | When `dualIso` present | Higher ISO value of the dual-ISO pair. |
| `dualIso.interleavePeriod` | integer | When `dualIso` present | Line interleave period (typically `2`). |
| `ettr` | object | Conditional | Only present when ETTR (`auto_ettr`) is enabled. |
| `ettr.lightLevel` | integer | When `ettr` present | Scene light level (0–255 scale from histogram analysis). |
| `ettr.sceneDR` | float | When `ettr` present | Estimated scene dynamic range in EV stops. |
| `ettr.highlightHeadroom` | float | When `ettr` present | Remaining highlight headroom in EV stops. |
| `ettr.channelClip` | array of 3 floats | When `ettr` present | Per-channel (R, G, B) clip ratios, range `[0.0..1.0]`. |
| `dualIsoPatch` | string | Always | MakerNote patch status: `"unsupported"` or `"applied"`. |

### Field Presence Summary

| Field | Present when |
|-------|-------------|
| `formatVersion` | Always |
| `schema_version` | Always |
| `filename` | Always |
| `lens` | Always |
| `pictureStyle` | Always |
| `dualIso` | Only when dual-ISO enabled AND active for this shot |
| `ettr` | Only when ETTR is enabled |
| `dualIsoPatch` | Always |

---

## `ml_export.json` Session Schema

**Writer:** `ai_export_session_write()` in `source-dev/modules/ettr/ai_lut.h`  
**Trigger:** Once at module init (`ettr_init()`), and on each state change from the `CBR_SHOOT_TASK` polling loop  
**Path:** `ML/DATA/ml_export.json`  
**Encoding:** UTF-8 text, valid JSON  
**One file per session (overwritten on state changes).**

### Field Reference

| Field | Type | Present when | Description |
|-------|------|-------------|-------------|
| `formatVersion` | integer | Always | Schema format generation. Currently `2`. |
| `schema_version` | string | Always | Semantic version `"<major>.<minor>"`. Currently `"2.0"`. |
| `firmwareVersion` | string | Always | Full ML_6D firmware build identifier. |
| `body` | string | Always | Camera body name (e.g. `"Canon EOS 6D"`). |
| `sensor` | object | Always | Sensor physical characteristics. |
| `sensor.pixelPitch` | float | Always | Pixel pitch in micrometers. Must be positive. |
| `sensor.cropFactor` | float | Always | Sensor crop factor relative to full-frame. Must be positive. |
| `session` | object | Always | Current session state at time of last write. |
| `session.dualIsoEnabled` | boolean | Always | Whether dual-ISO module is enabled globally. |
| `session.ettrEnabled` | boolean | Always | Whether ETTR (`auto_ettr`) is enabled globally. |
| `session.pictureStyle` | string | Always | Active Picture Style for the session. |

**All fields are always present.** The `session` block reflects the state
at time of last write (re-exported on any toggle change via the polling CBR).

---

## Degradation Rules

Every consumer failure path degrades to "standard CR2 import without AI
corrections." The user always gets their photos imported. AI corrections
are a bonus, never a gate.

| Condition | Behavior |
|-----------|----------|
| `ml_export.json` missing | Entire batch uses default processing; non-blocking user notice |
| `ml_export.json` unparseable (invalid JSON) | Same as missing |
| `ml_export.json` unknown `schema_version` major | Same as missing |
| `.ml6d` sidecar missing for a CR2 | That CR2 uses default processing (no notice — normal for pre-ML_6D shots) |
| `.ml6d` sidecar unparseable (invalid JSON) | Same as missing sidecar |
| `.ml6d` unknown `schema_version` major | Same as missing sidecar |
| `dualIsoPatch: "unsupported"` | Single-ISO CR2 handling for that shot; use sidecar `dualIso` block for detection instead of MakerNote inspection |
| Partial fields in sidecar (some optional blocks absent) | Apply what's present, default the rest |

**Key principles:**
- Import MUST NOT fail or block on any contract file issue.
- Import MUST NOT crash on unknown or missing schema versions.
- A missing sidecar is silent (expected for non-ML_6D cards).
- A missing session file surfaces a non-blocking toast/notice.

---

## Examples

### Example `.ml6d` Sidecar — ETTR + Dual-ISO Active

Based on `recovered_photos/IMG_5290.CR2` (Tamron SP 20-40mm, lens ID 160):

**File path:** `ML/DATA/SHOTS/IMG_5290.ml6d`

```json
{
  "formatVersion": 2,
  "schema_version": "2.0",
  "filename": "IMG_5290.CR2",
  "lens": {
    "id": 160,
    "caStrength": 15,
    "fringeReduce": 20
  },
  "pictureStyle": "STANDARD",
  "dualIso": {
    "enabled": true,
    "isoBase": 100,
    "isoAlternate": 1600,
    "interleavePeriod": 2
  },
  "ettr": {
    "lightLevel": 180,
    "sceneDR": 11.2,
    "highlightHeadroom": 0.8,
    "channelClip": [0.950, 0.920, 0.985]
  },
  "dualIsoPatch": "unsupported"
}
```

### Example `.ml6d` Sidecar — ETTR Disabled, No Dual-ISO

Based on a standard shot (lens ID 228):

**File path:** `ML/DATA/SHOTS/IMG_5296.ml6d`

```json
{
  "formatVersion": 2,
  "schema_version": "2.0",
  "filename": "IMG_5296.CR2",
  "lens": {
    "id": 228,
    "caStrength": 0,
    "fringeReduce": 0
  },
  "pictureStyle": "NEUTRAL",
  "dualIsoPatch": "unsupported"
}
```

Note: No `dualIso` block (dual-ISO not active). No `ettr` block (ETTR
disabled). Both omissions are correct per the field presence rules.

### Example `ml_export.json` — Session with ETTR Enabled

**File path:** `ML/DATA/ml_export.json`

```json
{
  "formatVersion": 2,
  "schema_version": "2.0",
  "firmwareVersion": "magiclantern-Nightly.2024Jul01.6D116",
  "body": "Canon EOS 6D",
  "sensor": {
    "pixelPitch": 6.54,
    "cropFactor": 1.0
  },
  "session": {
    "dualIsoEnabled": false,
    "ettrEnabled": true,
    "pictureStyle": "NEUTRAL"
  }
}
```

### Example `ml_export.json` — Session with Dual-ISO Enabled

```json
{
  "formatVersion": 2,
  "schema_version": "2.0",
  "firmwareVersion": "magiclantern-Nightly.2024Jul01.6D116",
  "body": "Canon EOS 6D",
  "sensor": {
    "pixelPitch": 6.54,
    "cropFactor": 1.0
  },
  "session": {
    "dualIsoEnabled": true,
    "ettrEnabled": true,
    "pictureStyle": "STANDARD"
  }
}
```

---

## Notes

- The firmware NEVER blocks or delays capture for metadata export. All
  write failures are silent or non-blocking notifications.
- `lens_tune.tbl` is NOT read by RaZStudio directly. The firmware bakes
  relevant per-lens values into the sidecar and session export at write time.
- `ML/DATA/SHOTS/` accumulates one `.ml6d` per capture with no built-in
  rotation. At ~500–600 bytes per sidecar, a 1000-shot session produces
  ~600 KB — negligible on any modern SD card.
- The `dualIsoPatch` field is always `"unsupported"` on EOS 6D until a
  verified MakerNote offset is determined (see `docs/VALIDATION.md`
  "Known Limitations" section).
- **AI White Balance accuracy caveat (OVF captures):** when the photo is
  taken through the optical viewfinder (not LiveView), the firmware's AI
  white-balance correction is computed from the *just-captured* frame
  during Canon image review and applied to the *next* capture — a
  one-shot lag inherent to the 6D (no live raw feed before an OVF
  exposure). Consequence for consumers: the WB actually baked into an
  OVF CR2 (`WB_RGGBLevelsAsShot`) may reflect the *previous* frame's
  scene, and in a rapidly changing scene can be visibly wrong (e.g. a dim
  frame carrying a brighter frame's gains → green cast). LiveView
  captures do not have this lag. RaZStudio should treat firmware WB as a
  starting hint, always user-overridable — which the contract already
  requires (see "WB Multipliers → Initial White Balance" below) — and
  not assume per-shot WB precision from OVF sessions. This is a hardware
  limitation, not a schema issue; no field change.
- Per-lens WB trims / picture-style `color_tone` in `lens_tune.tbl` are
  applied on top of the measured WB (and, for `color_tone`, the JPEG
  picture style when "AI Picture Tune" is on). A non-neutral user row can
  add its own tint; the shipped lens 160 row was neutralized 2026-07-21
  after it compounded a green JPEG cast on top of the OVF lag above.

---

## Import Adapter Interface

This section defines the contract-level interface that RaZStudio's import
module implements to consume the `.ml6d` sidecar and `ml_export.json`
session files written by the ML_6D firmware. The interface is documented
here as Kotlin signatures for reference; RaZStudio's internal
architecture may differ, but the behavioral contract MUST match.

### Interface Signatures

```kotlin
class Ml6dImportAdapter {
    /**
     * Called once per SD-card import batch.
     * Locates and parses ML/DATA/ml_export.json from the card root.
     * Returns session metadata or null (degraded mode — batch uses
     * default processing, non-blocking user notice).
     */
    fun loadSession(cardRoot: File): Ml6dSession?

    /**
     * Called per CR2. Locates ML/DATA/SHOTS/{basename}.ml6d and parses it.
     * Returns per-shot corrections or null (that CR2 uses default
     * processing — silent, no user notice).
     */
    fun loadSidecar(cr2File: File): Ml6dSidecar?

    /**
     * Apply corrections to a DevelopState. Non-destructive:
     * all values are initial/overridable, never baked.
     * If both session and sidecar are null, returns state unchanged.
     */
    fun applyCorrections(
        state: DevelopState,
        session: Ml6dSession?,
        sidecar: Ml6dSidecar?
    ): DevelopState
}
```

### Import Flow

1. **Batch start:** `loadSession(cardRoot)` is called once. If
   `ml_export.json` is missing, unparseable, or has an unknown major
   `schema_version`, returns `null` — the entire batch uses default
   processing and a non-blocking notice is surfaced.

2. **Per CR2:** `loadSidecar(cr2File)` is called for each CR2 in the
   import batch. If the `.ml6d` file is missing, unparseable, or has an
   unknown major `schema_version`, returns `null` — that CR2 uses
   default processing silently (no notice; expected for non-ML_6D cards).

3. **Apply:** `applyCorrections(state, session, sidecar)` merges
   firmware-derived values into the develop state as initial/overridable
   values. The user can override any applied correction in the develop
   UI exactly like any other source.

### Correction Application Rules

#### WB Multipliers → Initial White Balance

When the sidecar's `lens.wbR` and `lens.wbB` fields are present
(sourced from `lens_tune.tbl` columns `wb_r_trim` / `wb_b_trim`):

- Map sidecar `lens.wbR` → `DevelopState.initialWb.rMultiplier`
- Map sidecar `lens.wbB` → `DevelopState.initialWb.bMultiplier`
- These become the CR2's initial white balance state in RaZStudio's
  develop module
- The user can override them like any other WB source (shot WB, custom
  preset, manual picker)
- Values are **non-destructive** — they set the starting point, never
  baked into the pixel pipeline irreversibly

#### Lens Tune → Initial Picture Style

When the session's `ml_export.json` contains lens-tune-derived values
(from the active `lens_tune.tbl` entry):

- Map session `lensTune.contrast` → `DevelopState.initialPictureStyle.contrast`
- Map session `lensTune.saturation` → `DevelopState.initialPictureStyle.saturation`
- Map session `lensTune.colorTone` → `DevelopState.initialPictureStyle.colorTone`
- These become the CR2's initial picture style bias — a non-destructive
  starting point that the user can override or reset
- The lens tune values are per-session (same lens for all shots in a
  batch), not per-shot

#### Dual-ISO → Demosaic Flag

When the sidecar's `dualIso` block is present with `enabled: true`:

- Map `dualIso.isoBase` + `dualIso.isoAlternate` → flag the CR2 for
  dual-ISO demosaic in the rendering pipeline
- `dualIso.interleavePeriod` informs the demosaic algorithm's line
  interleave pattern

### `dualIsoPatch: "unsupported"` Handling

When a sidecar's `dualIsoPatch` field equals `"unsupported"`:

1. **Skip MakerNote inspection:** Do NOT attempt to read or interpret
   dual-ISO state from the CR2's Canon MakerNote IFD tags. The firmware
   did not patch the MakerNote, so any MakerNote-based dual-ISO
   detection would be unreliable or produce false negatives.

2. **Use sidecar as authoritative source:** The `.ml6d` sidecar's
   `dualIso` block (if present) is the sole authoritative source for
   dual-ISO state for that shot. If `dualIso.enabled: true` with valid
   `isoBase` and `isoAlternate`, the shot IS dual-ISO regardless of
   what the MakerNote says.

3. **No `dualIso` block + `"unsupported"`:** If the sidecar has
   `dualIsoPatch: "unsupported"` but no `dualIso` block, the shot is
   treated as single-ISO (standard CR2 demosaic). This is the expected
   state for non-dual-ISO shots on EOS 6D.

4. **Never fall back to MakerNote:** For any shot where
   `dualIsoPatch: "unsupported"`, MakerNote tag inspection for dual-ISO
   detection is explicitly prohibited — even if third-party tools might
   find dual-ISO indicators there, the firmware has not validated or
   patched those tags, so they cannot be trusted.

**Rationale:** On EOS 6D, the MakerNote dual-ISO offset has not been
verified via hex dump (see `docs/VALIDATION.md` "Known Limitations").
The `"unsupported"` status signals this explicitly so the consumer
never silently assumes the MakerNote is a reliable dual-ISO indicator.

### Degradation Summary (Import Adapter)

| Condition | `loadSession` | `loadSidecar` | `applyCorrections` |
|-----------|--------------|---------------|---------------------|
| Files present + valid | Returns `Ml6dSession` | Returns `Ml6dSidecar` | Applies all corrections as initial state |
| Session missing/invalid | Returns `null` | N/A | Applies sidecar-only corrections (if available) |
| Sidecar missing/invalid | N/A | Returns `null` | Returns state unchanged for that CR2 |
| Both missing | Returns `null` | Returns `null` | Returns state unchanged (full default processing) |
| Unknown major version | Returns `null` (rejected) | Returns `null` (rejected) | No corrections applied from rejected data |
| `dualIsoPatch: "unsupported"` | N/A | Returns sidecar normally | Uses sidecar `dualIso` block; skips MakerNote |
| Partial fields in sidecar | N/A | Returns partial sidecar | Applies present fields; defaults the rest |

**Key principle:** Import MUST NOT fail, block, or crash on any contract
file issue. AI corrections are a bonus, never a gate. The user always
gets their photos imported.

---

## Reference: ML_6D Custom ETTR Module vs Upstream Magic Lantern

This section documents the custom AI features in the ML_6D ETTR module
(`source-dev/modules/ettr/ettr.c` + `ai_lut.h`) that differentiate it
from the upstream Magic Lantern ETTR module. These are the firmware-side
features that produce the data consumed by RaZStudio.

### Upstream ML ETTR (stock features retained)

- Histogram-based expose-to-the-right metering (push highlights just below clip)
- Adjusts shutter/ISO in M mode (photo), LV standby (movie), and bulb
- Dual-ISO link (adjusts ISO spacing based on scene)
- SNR limits (midtone/shadow thresholds to avoid over-darkening)
- Overexposure recovery heuristics (percentile extrapolation)
- Debug overlay (metered areas visualization)

### Custom AI Features (ML_6D additions)

| Feature | Config Key | Source File | Description |
|---------|-----------|-------------|-------------|
| Auto ISO Optimizer | `auto_iso_optimizer` | ettr.c | Detects ISO=Auto, arms ETTR, enables HTP+ALO, sets floor ISO, runs `ai_lut_apply()` for learned scene corrections |
| AI-LUT Scene Classification | — | ai_lut.h (`ai_lut_apply`) | Loads `unified.tbl`, matches current scene by light level + classification, applies learned ISO/WB/HTP/ALO per-row |
| AI White Balance | `ai_white_balance` | ai_lut.h (`ai_white_point_wb`) | Per-half-press adaptive WB from RAW histogram white-point + gray-world estimation, confidence-blended |
| AI WB Warmth | `ai_wb_warmth` | ettr.c | Tunable amber bias (0=neutral → 4=warmest); auto-boosts under golden/warm light conditions |
| AI Picture Tune | `ai_picture_tune_en` | ai_lut.h (`ai_picture_tune`) | Per-lens contrast/saturation/color-tone from `lens_tune.tbl` → Canon PicStyle (JPEG only, non-destructive to RAW) |
| AI Lens Tune | on-camera editor | ai_lut.h | Full 9-column `lens_tune.tbl` load/save, per-lens EV bias, WB R/B trim, CA strength, fringe reduce |
| AI Data Logging | `ai_data_logging` | ai_lut.h (`ai_lut_log`) | Firmware-side logging to `ML/logs/unified_log.txt` on half-press (light level, ISO, WB, scene — for offline LUT training) |
| AI Modes | `ai_modes` | ettr.c | Extends AI coverage beyond M: `0` = P+M, `1` = P+M+Av+Tv |
| Extended ETTR Metadata | — | ettr.c (`ettr_compute_extended_metadata`) | Per-channel clip fractions (R/G/B), scene DR, highlight headroom — exported to `.ml6d` sidecar |
| Per-shot .ml6d Sidecar | — | ai_lut.h (`ai_sidecar_write`) | JSON sidecar per CR2 with all AI state, written via a `file_number`-change poll for every capture |
| Session Export | — | ai_lut.h (`ai_export_session_write`) | `ML/DATA/ml_export.json` with firmware/sensor/session state, re-exported on toggle changes |
| MakerNote Dual-ISO Patch | — | ai_lut.h (`ai_makernote_dualiso_patch`) | Writes dual-ISO flag into CR2 EXIF MakerNote IFD (when byte offset is known) |
| Post-Capture Poll | — | ettr.c (`auto_ettr_polling_cbr`, `CBR_SHOOT_TASK`) | Detects `file_number` changes on every poll tick; fires sidecar write + MakerNote patch on every NEW capture regardless of ETTR state. Replaced a `CBR_POST_SHOOT` handler confirmed dead on this fork (2026-07-21) — see the Sidecar Schema section above. |
| Per-lens EV Bias | `ai_lens_ev8` | ai_lut.h | ETTR target shifted per-lens from `lens_tune.tbl` (e.g., bloom-prone lens gets -1 EV protection) |
| Aggressive Default Target | `auto_ettr_target_level = 0` | ettr.c | Changed from upstream's -1 EV to -0.5 EV (closer to clip, maximizes DR) |

### Video Recording (.MOV) Behavior

The AI system's relationship to video recording:

| Phase | AI System Behavior |
|-------|-------------------|
| **Movie standby (LV, not recording)** | ETTR metering active — adjusts ISO/shutter. AI WB fires on half-press. ISO Optimizer sets HTP/ALO/floor. Lens tune loaded. All preparation happens here. |
| **Active H.264 recording** | ETTR stops adjusting (guarded by `NOT_RECORDING`). AI WB does not fire (requires half-press). No exposure jumps mid-clip. Session state is frozen at pre-record values. |
| **Record stop** | No hook fires. No sidecar written. No metadata exported for the .MOV file. |
| **Movie-specific shutter limit** | ETTR enforces shutter >= 1/fps (e.g., 1/24 at 24p). Without expo override, clamped to >= 1/30. |

**Current limitation:** No `.ml6d` sidecar or metadata file is produced
for `.MOV` video clips. The post-capture poll (`auto_ettr_polling_cbr`,
see above) watches still-image `file_number` on the CR2/DCIM card
counter; it has no equivalent trigger for video record-stop. The AI
system fully prepares camera state (WB, ISO, HTP, ALO, lens tune) before
recording starts, but does not persist what corrections were active for
the resulting video file.

**Future extension (not yet implemented):** To support RaZStudio video
import, the firmware would need to:
1. Hook `PROP_MVR_REC_START` (property 0x80030002, state→0 at record stop)
2. Write a `MVI_NNNN.ml6d` sidecar at record-stop with the session state
   that was active during that clip
3. Or extend `ml_export.json` with a `videoClips[]` array recording
   per-clip metadata (start file number, duration, active AI state)

Note: do not assume any given `CBR_*` constant actually fires on this
fork before depending on it — `CBR_PRE_SHOOT`/`CBR_POST_SHOOT` looked
identical to other working CBRs (same registration macro, same module.h
listing) but are never invoked anywhere in `source-dev/src`'s core. Grep
`module_exec_cbr(` call sites and confirm the specific type you need is
among them before building on it; a `file_number`- or property-change
poll on a CBR that IS proven to fire (e.g. `CBR_SHOOT_TASK`) is the
fallback pattern already used throughout this module.

### `lens_tune.tbl` Format Reference

9-column pipe-delimited file loaded by `ai_lens_tune_load()`:

```
lens_id|contrast|saturation|color_tone|ev_bias_8ths|wb_r_trim|wb_b_trim|ca_strength|fringe_reduce
254|1|-1|0|-8|980|1050|30|15
0|0|0|0|0|1024|1024|0|0  # default fallback (lens_id 0)
```

| Column | Type | Description |
|--------|------|-------------|
| `lens_id` | int | Canon lens ID (0 = default/unknown fallback) |
| `contrast` | int | Picture Style contrast bias (-4..+4) |
| `saturation` | int | Picture Style saturation bias (-4..+4) |
| `color_tone` | int | Picture Style color tone bias (-4..+4) |
| `ev_bias_8ths` | int | ETTR target shift in 1/8 EV steps (negative = more protective) |
| `wb_r_trim` | int | WB red multiplier (1024 = neutral) |
| `wb_b_trim` | int | WB blue multiplier (1024 = neutral) |
| `ca_strength` | int | Chromatic aberration correction strength (0-100) |
| `fringe_reduce` | int | Purple/green fringe suppression (0-100) |

RaZStudio does NOT read this file directly. The firmware bakes the
relevant per-lens values into the `.ml6d` sidecar (`lens` block) and
`ml_export.json` at write time. The table is documented here for
reference and debugging.

### `unified.tbl` Format Reference

Scene classification lookup table loaded by `ai_lut_load()`:

```
Scene|LightMin|LightMax|ISO|WB|ALO|HTP
Daylight_Bright|200|255|100|R100,G100,B100|OFF|ON
Daylight_Mid|128|199|200|R100,G100,B100|STD|ON
...
```

| Column | Type | Description |
|--------|------|-------------|
| `Scene` | string | Scene classification label (from training) |
| `LightMin` | int | Minimum light level for this row (0-255) |
| `LightMax` | int | Maximum light level for this row (0-255) |
| `ISO` | int | Target ISO for this scene |
| `WB` | string | WB multipliers as `R{n},G{n},B{n}` (G=100 reference) |
| `ALO` | string | Auto Lighting Optimizer setting: `OFF`, `LOW`, `STD`, `HIGH` |
| `HTP` | string | Highlight Tone Priority: `ON` or `OFF` |

The AI system matches the current scene by light level (from RAW
histogram) and applies the matched row's corrections. Fallback when no
LUT is present: ISO 400, neutral WB, HTP+ALO enabled (hardcoded
defaults in `ai_lut_apply()`).
