# Implementation Plan: RaZStudio Integration

## Overview

This plan implements the firmware-side fixes that enable every EOS 6D capture to produce a complete `.ml6d` sidecar and `ml_export.json` session file, documents the consumer contract for RaZStudio, and defines the on-device validation workflow. Firmware C changes come first (they're in this repo), followed by contract documentation and validation artifacts.

## Tasks

- [x] 1. Firmware call-site fix — fire sidecar on every capture (REQ-001)
  - [x] 1.1 Register CBR_POST_SHOOT in the ETTR module's CBR table
    - Add `MODULE_CBR(CBR_POST_SHOOT, ai_post_capture_cbr, 0)` to the `MODULE_CBRS_START()` block in `source-dev/modules/ettr/ettr.c` (after line 2308)
    - _Requirements: REQ-001 AC1_

  - [x] 1.2 Implement `ai_post_capture_cbr` handler in `ettr.c`
    - Write the `static unsigned int ai_post_capture_cbr(unsigned int ctx)` function
    - Construct basename without `.CR2` extension: `snprintf(basename, sizeof(basename), "IMG_%04d", file_number)`
    - Construct full CR2 name for the dual-ISO patch: `snprintf(cr2_name, sizeof(cr2_name), "IMG_%04d.CR2", file_number)`
    - Call `ai_sidecar_write(basename)` (note: passes basename, not full filename — fixes the double-extension bug)
    - Call `ai_makernote_dualiso_patch(cr2_name, AI_MKNOTE_DUALISO_OFFSET_TODO)` if offset > 0
    - Return 0
    - Place this function near the existing `auto_ettr_photo_task()` (around line 968) for locality
    - _Requirements: REQ-001 AC1, REQ-001 AC3_

  - [x] 1.3 Remove the ETTR-only sidecar call at ettr.c:986
    - Delete the `ai_sidecar_write(cr2_name)` call inside `auto_ettr_photo_task()` (and the surrounding `char cr2_name[16]; snprintf(...)` block that constructs it for the sidecar)
    - Keep the ETTR re-metering logic (`auto_ettr_get_correction()` / `auto_ettr_step_task()`) intact
    - Keep the `ai_makernote_dualiso_patch` call removal from this function too (now handled by the new CBR)
    - _Requirements: REQ-001 AC2_

  - [x] 1.4 Update the misleading comment at ettr.c:1003-1008
    - Replace the comment block in `auto_ettr_step()` that says "no dedicated capture-complete hook was found" with an accurate note: `CBR_POST_SHOOT` is now registered and handles sidecar/MakerNote export for all captures
    - _Requirements: REQ-001 AC1_

- [x] 2. Fix sidecar path and extension (REQ-003, Property 3)
  - [x] 2.1 Rename sidecar extension from `.ml` to `.ml6d` in `ai_sidecar_write()`
    - In `source-dev/modules/ettr/ai_lut.h`, change the format string at line 822 from `"%s/%s.ml"` to `"%s/%s.ml6d"`
    - _Requirements: REQ-003 AC1_

  - [x] 2.2 Verify basename handling produces correct path
    - Confirm that the new `ai_post_capture_cbr` (task 1.2) passes `"IMG_NNNN"` (without `.CR2`) to `ai_sidecar_write()`, so the output path becomes `ML/DATA/SHOTS/IMG_NNNN.ml6d` (not `IMG_NNNN.CR2.ml6d`)
    - Update the `"filename"` field in the JSON body to still record the full CR2 name — pass it as a separate parameter or reconstruct inside the writer
    - _Requirements: REQ-003 AC1, Property 3_

- [x] 3. Add `schema_version` field to both writers (REQ-003, REQ-004)
  - [x] 3.1 Add `"schema_version":"2.0"` to `ai_sidecar_write()` format string
    - In `ai_lut.h`, modify the `snprintf` format in `ai_sidecar_write()` to include `"schema_version":"2.0"` immediately after `"formatVersion":2`
    - Verify buffer sizes remain adequate (current json buffer is 640 bytes — adding ~22 chars is safe)
    - _Requirements: REQ-003 AC3, Property 1_

  - [x] 3.2 Add `"schema_version":"2.0"` to `ai_export_session_write()` format string
    - In `ai_lut.h`, modify the `snprintf` format in `ai_export_session_write()` to include `"schema_version":"2.0"` after `"formatVersion":2`
    - Verify the 512-byte json buffer remains adequate
    - _Requirements: REQ-004 AC2, Property 2_

- [x] 4. Add `dualIsoPatch` status field to sidecar (REQ-002)
  - [x] 4.1 Add conditional `dualIsoPatch` field emission in `ai_sidecar_write()`
    - After the existing `dualIso` block (and after the `ettr` block), add logic:
      - If `AI_MKNOTE_DUALISO_OFFSET_TODO == 0`: emit `,"dualIsoPatch":"unsupported"`
      - Else: emit `,"dualIsoPatch":"applied"`
    - This field is emitted ALWAYS (regardless of dual-ISO state) per the design's field presence table
    - Verify buffer capacity (640 bytes still sufficient with ~30 additional chars)
    - _Requirements: REQ-002 AC1, REQ-002 AC3, Property 1_

  - [x] 4.2 Document the unsupported status in `docs/VALIDATION.md`
    - Add a "Known Limitations" section (or append to existing) noting: "Dual-ISO MakerNote patch: UNSUPPORTED on EOS 6D (offset TBD). RaZStudio uses the .ml6d sidecar `dualIso` block for detection."
    - _Requirements: REQ-002 AC3_

- [x] 5. Checkpoint — Firmware changes compile and are internally consistent
  - Ensure all firmware changes in tasks 1–4 compile cleanly with the existing ML_6D ARM cross-compiler toolchain (`make` from `source-dev/`). Verify no new warnings. Ask the user if questions arise.

- [x] 6. RaZStudio contract documentation (REQ-005)
  - [x] 6.1 Create contract specification file `docs/RAZSTUDIO_CONTRACT.md`
    - Document the `.ml6d` sidecar JSON schema (all fields, types, presence rules) exactly as defined in the design's Data Models section
    - Document the `ml_export.json` session schema
    - Document schema version semantics (major.minor, rejection rules)
    - Document degradation rules table (all failure modes → behavior)
    - Include example JSON for both files using real field values from `recovered_photos/`
    - _Requirements: REQ-003, REQ-004, REQ-005_

  - [x] 6.2 Document the import adapter interface contract
    - In the same `docs/RAZSTUDIO_CONTRACT.md`, add an "Import Adapter Interface" section with the Kotlin interface signatures (`loadSession`, `loadSidecar`, `applyCorrections`)
    - Document correction application rules: WB multipliers → initial WB state (overridable), lens tune → initial picture style (overridable)
    - Document `dualIsoPatch: "unsupported"` handling (fall back to sidecar `dualIso` block, no MakerNote inspection)
    - _Requirements: REQ-005 AC1, REQ-005 AC2, REQ-005 AC3, REQ-005 AC4_

  - [ ]* 6.3 Write contract validation unit tests (example-based)
    - Create a test file (Python or shell script using `jq`) that validates sample `.ml6d` and `ml_export.json` files against the documented schema
    - Test cases: ETTR-disabled sidecar has no `ettr` block; dual-ISO disabled has no `dualIso` block; `dualIsoPatch` is always present; `schema_version` is always `"2.0"`
    - Use sample files generated from `recovered_photos/` CR2s once firmware is flashed
    - _Requirements: REQ-003 AC2, REQ-003 AC3, Property 1, Property 2_

- [x] 7. Checkpoint — Contract documentation is complete and consistent
  - Review `docs/RAZSTUDIO_CONTRACT.md` against the design document. Ensure all fields, types, and degradation rules match. Ask the user if questions arise.

- [x] 8. End-to-end validation artifacts (REQ-006)
  - [x] 8.1 Create validation test script `source-dev/build_tools/validate_sidecar.py`
    - Python script that takes a directory path and validates:
      - Every `.CR2` file has a matching `.ml6d` in `ML/DATA/SHOTS/`
      - Each `.ml6d` parses as valid JSON
      - Each `.ml6d` contains required fields (`formatVersion`, `schema_version`, `filename`, `lens`, `pictureStyle`, `dualIsoPatch`)
      - `schema_version` equals `"2.0"`
      - `ettr` block present only when expected (check against a flag or just validate structure)
      - `ml_export.json` exists and parses correctly
    - Output pass/fail summary suitable for `docs/VALIDATION.md`
    - _Requirements: REQ-006 AC3, Property 1, Property 3_

  - [x] 8.2 Update `docs/VALIDATION.md` with RaZStudio integration test protocol
    - Add a "RaZStudio Integration Validation" section following the same format as the `ai-lut-magic-lantern` spec's Task 10
    - Document the test sequence: build → flash → capture (non-ETTR, ETTR, dual-ISO) → pull card → run `validate_sidecar.py` → import into RaZStudio/test harness
    - Include placeholder table for recording pass/fail results per capture type
    - Reference `exiftool` commands needed to inspect MakerNote and sidecar fields
    - _Requirements: REQ-006 AC1, REQ-006 AC2, REQ-006 AC3, REQ-006 AC4, REQ-006 AC5_

  - [ ]* 8.3 Write property-based test stubs for RaZStudio import adapter
    - Create `tests/pbt/razstudio_contract_pbt.py` with Hypothesis-based property tests (Python stub since RaZStudio Kotlin tests are out-of-scope for this repo):
      - **Property 1: Sidecar writer produces valid, complete JSON** — generate random firmware states, serialize to JSON, verify structure
      - **Property 3: Sidecar path follows naming convention** — generate random valid CR2 filenames, verify path construction equals `ML/DATA/SHOTS/{basename}.ml6d`
      - **Property 4: Import adapter graceful degradation** — generate broken/missing/outdated contract files, verify no exception + valid defaults returned
      - **Property 5: Schema version rejection** — generate `schema_version` strings with major > 2, verify rejection
    - Mark as reference implementation; actual Kotlin PBTs belong in RaZStudio repo
    - _Requirements: REQ-003, REQ-005, Properties 1, 3, 4, 5_

- [x] 9. Final checkpoint — All artifacts complete
  - Ensure all firmware changes compile, contract documentation is complete, validation script runs against sample data, and `docs/VALIDATION.md` is updated. Ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Firmware changes (tasks 1–4) must be completed on the `ai-lut-integration` branch
- All C code must stay within ML_6D constraints: ARM Cortex-R4, integer-only on shared paths, no new heap allocation
- The firmware MUST NEVER block or delay capture for metadata export — all write failures are silent or non-blocking
- RaZStudio internal implementation is out of scope; only the contract interface is documented here
- If `CBR_POST_SHOOT` proves unreliable on-device (fires before CR2 is closed), fall back to `PROP_LAST_JOB_STATE` with `job_state==0` guard
- Property tests for RaZStudio consumer logic (Properties 4, 5, 6) belong in the RaZStudio repository; task 8.3 provides Python reference stubs only
- The `AI_MKNOTE_DUALISO_OFFSET_TODO` define intentionally stays at 0 until a real hex-dump-verified offset is found via `exiftool -v3` on a dual-ISO CR2

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1", "2.1", "3.1", "3.2"] },
    { "id": 1, "tasks": ["1.2", "2.2", "4.1"] },
    { "id": 2, "tasks": ["1.3", "1.4", "4.2"] },
    { "id": 3, "tasks": ["6.1"] },
    { "id": 4, "tasks": ["6.2", "6.3"] },
    { "id": 5, "tasks": ["8.1", "8.2"] },
    { "id": 6, "tasks": ["8.3"] }
  ]
}
```
