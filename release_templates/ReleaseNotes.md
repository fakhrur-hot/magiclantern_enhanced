# AI Magic Lantern -- Automated Build

**Upstream commit:** __COMMIT__
**Build date:** __DATE__

## Camera models
__CAMERAS__

## Missing camera binaries
__MISSING__

## What's included
- `magiclantern.bin` per camera, under `EOS-<model>/`
- `unified.tbl` AI-LUT, copied into each camera folder

## Known issues
- ALO (`apply_alo`) and HTP (`apply_htp`) are Phase 2 stubs -- documented no-ops
  until the Magic Lantern hooks are confirmed (tasks.md Tasks 11-12).
- On-camera Lua API names are reconciled during on-camera testing (Task 10).
- Builds use `make || true`; a missing binary above means that camera's build
  failed in CI and needs investigation.

## Installation
See `INSTALL.md`. Copy the Lua scripts to `A:/ML/scripts/` and `unified.tbl`
to `A:/ML/models/`.
