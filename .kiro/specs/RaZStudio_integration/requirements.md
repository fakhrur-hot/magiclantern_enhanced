# Requirements Document

## Introduction

This project connects the ML_6D firmware's on-camera AI intelligence
(AI-LUT white balance, lens tuning, ETTR metadata, dual-ISO detection --
see [[ai-lut-magic-lantern]] and the `ml6d-extended-intelligence` /
`cr2-intelligence-integration` specs) to **RaZStudio**, the companion
Android RAW-processing app, so that every CR2 captured on the EOS 6D
carries firmware-computed metadata that RaZStudio consumes automatically
when the file is imported.

A prior audit (`cr2-intelligence-integration` spec) found the firmware
side mostly implemented in `source-dev/modules/ettr/ai_lut.h` /
`ettr.c` (`ai_export_session_write()`, `ai_sidecar_write()`,
`ai_makernote_dualiso_patch()`), but flagged two concrete defects and
zero corresponding consumer code:

1. `ai_sidecar_write()` is only ever called from `auto_ettr_photo_task()`
   (`ettr.c:986`), so a per-shot `.ml6d` sidecar is written **only while
   ETTR is active** -- not for every CR2 capture as intended.
2. `ai_makernote_dualiso_patch()` is gated behind
   `AI_MKNOTE_DUALISO_OFFSET_TODO` (`ettr.c:966`), hardcoded to `0`,
   making the dual-ISO MakerNote patch a permanent no-op.
3. No Android/Kotlin code exists anywhere in the `ML_6D` repository.
   RaZStudio is a separate repository/app; this spec defines the
   **contract** RaZStudio must implement against, not RaZStudio's
   internal implementation.

This spec closes that consumer gap: it defines the on-disk contract
(`ML/DATA/ml_export.json` session file + per-shot `.ml6d` sidecars) as
consumed by RaZStudio, the fixes required on the firmware side so the
contract is honored for every capture, and the on-device + on-import
validation loop that proves the pipeline works end to end.

All firmware work continues on the `ai-lut-integration` branch. RaZStudio
consumption is validated by importing real SD-card output; this spec
does not modify RaZStudio's UI/rendering pipeline beyond the import
adapter needed to read the contract.

---

## Requirements

### REQ-001 -- Fix Sidecar Export to Fire on Every Capture

**User Story:** As a RaZStudio user, I want every CR2 I shoot -- not just
ETTR-triggered shots -- to have a matching `.ml6d` sidecar, so that
RaZStudio can apply firmware-computed corrections regardless of shooting
mode.

**Acceptance Criteria:**

- `ai_sidecar_write(cr2_filename)` is invoked from the camera's general
  post-capture hook (the same point where the CR2 file is finalized on
  the card), not only from `auto_ettr_photo_task()`.
- `auto_ettr_photo_task()` keeps calling `ai_sidecar_write()` for ETTR
  shots, but the ETTR-only call site in `ettr.c:986` is no longer the
  sole trigger -- it must not fire twice for the same CR2.
- A shot taken with ETTR disabled still produces a `.ml6d` sidecar with
  whatever fields are computable without ETTR context (WB, lens tune,
  scene metadata); ETTR-only fields are omitted or set to a documented
  neutral default, never left as stale data from a prior shot.
- No change to sidecar file format (REQ-003) is required to satisfy this
  requirement -- this is a call-site fix, not a schema change.

---

### REQ-002 -- Resolve or Explicitly Defer Dual-ISO MakerNote Patch

**User Story:** As a developer, I want the dual-ISO MakerNote patch to
either work correctly or be clearly and safely disabled, so that
RaZStudio never receives a corrupted or silently-wrong MakerNote offset.

**Acceptance Criteria:**

- `AI_MKNOTE_DUALISO_OFFSET_TODO` is replaced with a real, hex-dump-verified
  offset for the EOS 6D's Canon MakerNote dual-ISO tag, **or** the call
  site is left disabled with a `.ml6d` sidecar field
  `"dual_iso_patch": "unsupported"` so RaZStudio can detect the
  limitation instead of assuming a patch was applied.
- If a real offset is found, `ai_makernote_dualiso_patch()` is validated
  against at least one real dual-ISO CR2 file using `exiftool` to confirm
  the MakerNote tag decodes correctly after patching.
- If left unsupported, this is documented in `docs/VALIDATION.md` (or
  equivalent) as a known limitation, not silently absent.
- RaZStudio must never crash or silently misinterpret dual-ISO data when
  `dual_iso_patch` is `"unsupported"` -- it falls back to standard
  single-ISO CR2 handling.

---

### REQ-003 -- `.ml6d` Sidecar Contract

**User Story:** As a RaZStudio developer, I want a documented, versioned
sidecar file format written alongside every CR2, so that RaZStudio can
parse firmware metadata without guessing field names or types.

**Acceptance Criteria:**

- Sidecar file is named `<CR2_BASENAME>.ml6d` and written to the same
  directory as the CR2 (or a documented fixed subdirectory, e.g.
  `ML/DATA/sidecars/`) at capture time via `ai_sidecar_write()`.
- File is valid UTF-8 text (JSON or key=value -- match whatever
  `ai_sidecar_write()` currently emits; do not invent a new format).
- Sidecar includes a `"schema_version"` field. RaZStudio's parser rejects
  or safely ignores sidecars with an unrecognized major version rather
  than misparsing fields.
- Documented fields (superset, some may be omitted per REQ-001):
  scene classification, WB multipliers (R/G/B, G=100 reference), lens
  tune identifier (from `lens_tune.tbl`, 9-column format), ETTR exposure
  metadata (when ETTR fired), `dual_iso_patch` status (REQ-002).
- Missing/unparseable sidecar is a non-fatal condition for RaZStudio --
  the CR2 still imports and renders with default (non-AI) processing.

---

### REQ-004 -- `ml_export.json` Session Contract

**User Story:** As a RaZStudio developer, I want a single per-session
export file describing camera/lens/model state, so that RaZStudio can
apply session-wide corrections (e.g. lens tuning) without re-deriving
them per shot.

**Acceptance Criteria:**

- `ai_export_session_write()` writes `ML/DATA/ml_export.json` at session
  start and on each subsequent invocation point already present in
  `ettr.c` (`ettr.c:1970`, `ettr.c:2294`).
- File contains at minimum: firmware/build identifier, active lens
  identifier matched against `lens_tune.tbl`, WB calibration state, and
  a timestamp.
- RaZStudio's import adapter reads this file once per SD-card import
  batch (not per CR2) and caches the parsed session state for that
  import.
- If `ml_export.json` is absent or unparseable, RaZStudio imports the
  batch using default (non-AI) processing for every CR2 in it, and
  surfaces a non-blocking notice to the user that session metadata was
  unavailable.

---

### REQ-005 -- RaZStudio Import Adapter (Contract-Level)

**User Story:** As a RaZStudio user, I want AI-derived corrections
(white balance, lens tuning) to apply automatically the moment I import
CR2 files from an ML_6D card, so that I don't have to configure anything
manually.

**Acceptance Criteria:**

- On import of a folder/card, RaZStudio locates `ml_export.json` first
  (REQ-004), then for each CR2 looks for a matching `.ml6d` sidecar
  (REQ-003).
- WB multipliers from the sidecar are applied as the CR2's initial WB
  state in RaZStudio's develop module, overridable by the user like any
  other WB source.
- Lens-tune-derived contrast/saturation bias (from `lens_tune.tbl` via
  the session export) is applied as a starting point for the picture
  style equivalent in RaZStudio, not baked in irreversibly.
- Import must not fail or block when the firmware contract files are
  partially present, absent, or from an older `schema_version` --
  degraded/default processing is always the fallback path (see REQ-003,
  REQ-004).
- This requirement defines the contract only; RaZStudio's internal
  develop-module architecture is out of scope here.

---

### REQ-006 -- End-to-End On-Device Validation

**User Story:** As a developer, I want a documented, repeatable test
that proves a real CR2 shot on the EOS 6D produces a sidecar and session
file that RaZStudio correctly imports, so that "done" means verified,
not just "compiles."

**Acceptance Criteria:**

- Build firmware with the REQ-001/REQ-002 fixes and flash to a physical
  EOS 6D test card (see `[[ml6d-deploy-matched-pair]]` -- `autoexec.bin`
  + `.sym` must be deployed together).
- Capture at least: one non-ETTR shot, one ETTR shot, one dual-ISO shot
  (if hardware/mode supports it).
- Verify with `exiftool` and manual sidecar inspection that each CR2 has
  correct WB/lens/scene fields matching real shooting conditions.
- Import the resulting card contents into RaZStudio (or a stub/test
  harness implementing REQ-005's contract) and confirm WB and lens-tune
  corrections appear as expected without manual intervention.
- Record pass/fail results and any Lua/C API mismatches found in
  `docs/VALIDATION.md`, following the same format used for the
  `ai-lut-magic-lantern` spec's Task 10.

---

## Non-Functional Requirements

- **Firmware constraints:** All firmware-side changes (REQ-001, REQ-002)
  stay within existing ML_6D constraints -- EOS 6D DIGIC 5+ ARM Cortex-R4,
  integer-only math on any code path shared with Lua, no new floating
  point in `ai_lut.h`/`ettr.c` beyond what already exists there.
- **Backward compatibility:** `schema_version` bump required for any
  field addition/removal in `.ml6d` or `ml_export.json`; RaZStudio must
  never crash on an unknown or missing version.
- **Non-blocking degradation:** Every RaZStudio-side failure mode
  (missing file, bad schema, unparseable field) degrades to default
  processing -- never a failed import, never a crash.
- **No repo coupling:** This spec does not require RaZStudio's source to
  live in or be vendored into the `ML_6D` repository; the contract
  (file names, fields, schema version) is the integration surface.

---

## Glossary

- **RaZStudio:** The companion Android RAW-processing app that imports
  CR2 files shot on ML_6D-flashed cameras and applies firmware-derived
  AI corrections.
- **`.ml6d` sidecar:** Per-CR2 metadata file written by
  `ai_sidecar_write()` at capture time (REQ-003).
- **`ml_export.json`:** Per-session metadata file written by
  `ai_export_session_write()` (REQ-004).
- **Schema version:** Integer/string tag in both contract files allowing
  RaZStudio to detect and safely handle firmware/consumer version drift.
- **Degraded import:** RaZStudio importing a CR2 with default
  (non-AI-corrected) processing because contract files were missing,
  malformed, or version-mismatched.
