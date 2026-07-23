# ML_6D Project Context & User Data

## User Profile

- **Username:** raze
- **Machine:** Windows (win32), cmd shell
- **WSL:** Available with `arm-none-eabi-gcc` 13.2.1 installed at `/usr/bin/`
- **Docker:** Available
- **SD Card Mount:** D:\ (EOS 6D SD card or Android device with ML deployed)
- **Working Directory:** `C:\Users\Public\Kiro\ML_6D`

## Project Overview

This is a **Magic Lantern fork for the Canon EOS 6D** (firmware 1.1.6) with AI-enhanced photography features. The project adds machine-learning-derived corrections (white balance, lens tuning, ETTR exposure) that are computed on-camera and consumed by **RaZStudio**, a companion Android RAW processing app.

### Key Architecture

- **Firmware:** ARM Cortex-R4 (DIGIC 5+), integer-only on shared paths, no new heap allocation
- **Platform:** `source-dev/platform/6D.116/`
- **AI Module:** `source-dev/modules/ettr/ettr.c` + `ai_lut.h` (sidecar writer, session export, lens tuning, ETTR metadata)
- **Lua Scripts:** `lua_scripts/unified_logger.lua`, `lua_scripts/decision_engine.lua`
- **Models:** `models/unified.tbl` (scene LUT), `models/lens_tune.tbl` (9-column pipe-delimited lens correction table)
- **Training:** `colab/lut_training.ipynb` (Google Colab notebook for LUT training)

### On-Card Deployment Structure (D:\)

```
D:\autoexec.bin          — ML firmware binary
D:\ML-SETUP.FIR          — Setup firmware image
D:\ML\modules\ettr.mo    — ETTR module (includes AI-LUT + sidecar writer)
D:\ML\modules\6D_116.sym — Symbol file (must match autoexec.bin)
D:\ML\modules\dual_iso.mo, lua.mo, etc.
D:\ML\models\unified.tbl — Scene classification LUT
D:\ML\models\lens_tune.tbl — Per-lens correction table
D:\ML\logs\unified_log.txt — Runtime decision log
D:\ML\DATA\SHOTS\*.ml6d  — Per-shot sidecar files (created at capture time)
D:\ML\DATA\ml_export.json — Session metadata file
```

## Build Process

```powershell
# Build from PowerShell using WSL:
wsl bash -c "cd /mnt/c/Users/Public/Kiro/ML_6D/source-dev && make -C platform/6D.116 -j4"

# Output at: source-dev/platform/6D.116/build/
#   autoexec.bin, 6D_116.sym, magiclantern.zip, modules/ettr.mo

# Deploy to SD card:
Expand-Archive -Path "source-dev\platform\6D.116\build\magiclantern.zip" -DestinationPath "D:\" -Force
Copy-Item "source-dev\platform\6D.116\build\autoexec.bin" -Destination "D:\autoexec.bin" -Force
```

## Completed Specs

### 1. ai-lut-magic-lantern (completed previously)
- AI-LUT integration: scene classification, WB/ISO decisions from unified.tbl
- ETTR exposure automation
- Lua-based unified logger and decision engine
- On-device validation workflow

### 2. RaZStudio_integration (completed 2026-07-20, sidecar trigger corrected 2026-07-21)
- **Firmware fixes:**
  - Fixed double-extension bug (was `IMG_NNNN.CR2.ml` → now `IMG_NNNN.ml6d`)
  - Fixed `file_number` access (uses `get_shooting_card()->file_number` for module safety)
  - Added `schema_version: "2.0"` to both sidecar and session writers
  - Added `dualIsoPatch` field (always emitted: "unsupported" or "applied")
  - Removed duplicate ETTR-only sidecar call (single call site now)
- **Contract documentation:** `docs/RAZSTUDIO_CONTRACT.md`
  - .ml6d sidecar JSON schema (all fields, types, presence rules)
  - ml_export.json session schema
  - Schema version semantics (major.minor, rejection rules)
  - Degradation rules (8 failure modes → graceful fallback)
  - Import adapter interface (Kotlin signatures, correction application rules)
- **Validation:** `source-dev/build_tools/validate_sidecar.py`
  - Batch-validates sidecars against contract schema
  - `docs/VALIDATION.md` updated with integration test protocol
- **Known limitation:** Dual-ISO MakerNote offset is TBD (define stays at 0, sidecar says "unsupported")
- **CORRECTED 2026-07-21 (real on-camera regression, found via field test):** the sidecar trigger
  was originally implemented as a `CBR_POST_SHOOT` module callback (`ai_post_capture_cbr`),
  on the assumption this fork fires that event after every capture like mainline Magic Lantern.
  **That assumption was wrong.** Grepping every `module_exec_cbr()` call site in `source-dev/src`
  shows `CBR_PRE_SHOOT`/`CBR_POST_SHOOT` are never invoked anywhere in this fork's core — the
  handler was dead code that could never run. Confirmed by shooting 4 real CR2s: `ML/DATA/SHOTS`
  never got created at all, not even for ETTR shots (worse than the original "ETTR-only"
  limitation — this had regressed to "never fires for anything"). Also means Lua's own
  `event.post_shoot` (registered the same dead way in `lua.c`) doesn't work on this build either
  — a separate, pre-existing gap, not introduced by this project.
  **Fix:** removed `ai_post_capture_cbr`/`CBR_POST_SHOOT` entirely; sidecar + MakerNote-patch
  writing now happens via a `file_number`-change poll inside `auto_ettr_polling_cbr`
  (`CBR_SHOOT_TASK`, proven to actually fire — same mechanism `ai_export_session_write()`'s
  polling already used, whose own comment had already flagged this exact gap). Fires for every
  new capture regardless of shooting mode/ETTR state, with a first-poll guard so module load /
  card swap doesn't spuriously sidecar the pre-existing last shot on the card.

## Key Files Modified (RaZStudio Integration)

| File | Changes |
|------|---------|
| `source-dev/modules/ettr/ettr.c` | Removed dead `CBR_POST_SHOOT`/`ai_post_capture_cbr`; sidecar + MakerNote patch now fire from a `file_number`-change poll in `auto_ettr_polling_cbr` (`CBR_SHOOT_TASK`) |
| `source-dev/modules/ettr/ai_lut.h` | .ml6d extension, basename handling, schema_version in both writers, dualIsoPatch field |
| `docs/RAZSTUDIO_CONTRACT.md` | New file — complete contract specification |
| `docs/VALIDATION.md` | Added Known Limitations + RaZStudio Integration Validation sections |
| `source-dev/build_tools/validate_sidecar.py` | New file — schema validation script |

### 3. Session 2026-07-21 — sd_uhs / lua / MLV-ETTR audit and fixes
- **`sd_uhs.c`:** added an "Auto Speed Test" wizard (`Prefs > SD Overclock > Auto Speed Test`) —
  cycles 240→192→160MHz across reboots, auto-detects Canon's safe-mode fallback via the existing
  `0xC0400614` register check, but deliberately requires the user to confirm real recording
  stability (cannot be auto-verified — benchmark-passing cards can still fail in sustained use,
  per community field data). Logs to `ML/logs/sd_autotest.txt`. **Never tested on real hardware.**
- **`modules/lua/lua.c`:** fixed `LUA_CBR_FUNC(vsync)`/`(display_filter)`/`(vsync_setparam)` —
  these were missing their required `(arg, timeout)` macro parameters, a bug inherited from
  upstream Magic Lantern itself (confirmed against Danne's Bitbucket mirror) that had never
  surfaced because `CONFIG_VSYNC_EVENTS` is essentially never enabled anywhere upstream. Fixed
  with `timeout=0` (non-blocking) since `CBR_VSYNC` fires every LiveView frame.
- **`modules/lua/Makefile`:** the `lua.sym` build rule blanket-hides ALL Lua C API symbols from
  other modules ("we have nothing interesting to export yet"). `ettr.mo`'s `gyro_bridge.c` (video-
  gyro-metadata work) needs `lua_pushinteger`/`lua_pushnumber`/`lua_pushstring`/`lua_setfield`/
  `lua_createtable` cross-module — added an explicit exclusion list so exactly those 5 stay
  exported. Without this, loading `ettr.mo` failed at boot: `tcc: error: undefined symbol
  'lua_pushinteger'`.
- **`modules/ettr/mlv_metadata.c`:** replaced a call to `mlv_set_type()` (a plain hard `extern`
  from `mlv_rec/mlv.h`, not the `WEAK_FUNC` pattern this codebase uses for optional cross-module
  calls) with a trivial local 4-byte `blockType` copy — removes a dependency that would fail to
  link whenever `mlv_lite`/`mlv_rec` isn't enabled (`mlv_lite.en` was absent on the test card).
- **Whole-project symbol audit:** cross-checked every included module's unresolved externs against
  core firmware + all other modules' exports. Method: `comm -23` between each `.dep` and the
  union of `.sym` files. Confirmed clean after the two fixes above (initial pass had 2 false
  positives from a module-path mapping bug in the audit script itself, not real issues).
- **`ai_lut.h` AI White Balance — attempted and REVERTED green-magenta correction:** tried driving
  `lens_set_wbs_gm()` from the product `r_hi*b_hi` (a `log(a)+log(b)=log(a*b)` trick meant to
  isolate a green/magenta cast from ordinary color-temperature variation, anchored to a single
  daylight calibration point). **Field-disproven**: real test shot at 4100K came back with
  `WB Shift GM: +4` (Canon: positive = toward green) and the JPEG was severely, overwhelmingly
  green — confirmed via a 4-shot burst showing the value ratchet 0→0→2→4 (this code's own
  ±2/press damping compounding in the wrong direction, since the single-anchor assumption breaks
  down away from daylight). Reverted; do not re-attempt without either multiple calibration
  anchors spanning tungsten..daylight..shade, or a fundamentally different signal. See
  `[[ml6d-ai-wb-picture-tune]]` memory for the full history — this is the first WB fix in this
  file's history that skipped real field-log validation before landing, and it broke immediately.
- **Real per-shot log/photo audit** (unified_log.txt, `ml_export.json`, CR2 EXIF via exiftool)
  is now the standard way to validate any WB/ETTR/sidecar change in this project — every fix
  above was found or confirmed this way, not by code inspection alone. (exiftool available at
  `C:\Program Files\ShareX\exiftool.exe`; no WSL/native install needed.)

### 4. Session 2026-07-21 (cont.) — AI WB "still green" investigation
Field complaint: JPEGs (esp. dim indoor / white-tile scenes) render heavily green even after
resetting Canon's WB Shift. Diagnosed via CR2 EXIF (exiftool) + `unified_log.txt`:
- **Root cause is the OVF/QR one-shot lag, not the estimator math.** `ai_qr_task` (ettr.c) runs
  on `PROP_GUI_STATE == GUISTATE_QR` — the image-REVIEW state, i.e. AFTER a photo is already
  captured. It meters the just-taken frame and applies custom WB gains, but those can only affect
  the NEXT capture. So in OVF shooting AI WB is always exactly one shot behind: a dim frame
  inherits the previous (brighter) frame's higher R/B gains, which suppress red+blue → green.
  This fully explains the persistent log-vs-EXIF mismatch (log records the correction for the
  next frame; each CR2 carries the previous frame's correction — they never line up) and
  `WhiteBalance=Auto` on the first shot of a burst. **Hardware limitation** (6D OVF has no live
  raw feed pre-capture); LiveView shooting avoids it (half-press meters the same frame captured).
  DECISION: accepted as a known limitation, not code-fixed.
- **Per-lens custom row was a secondary contributor.** `models/lens_tune.tbl` lens 160 (Tamron,
  the mounted lens) had a `#user` row `160|-1|0|1|-8|960|1152|15|20` — `color_tone +1` (JPEG
  yellow-green shift, active because `auto.ettr.ai.pictune=1`) + `wb_r 960 / wb_b 1152` (warm WB
  trims). Too small to cause neon-green alone, but stacked on the OVF lag. **NEUTRALIZED**
  2026-07-21 → `160|-1|0|0|-8|1024|1024|15|20` in BOTH the repo copy and the card copy
  (`D:\ML\models\lens_tune.tbl`). Kept ev_bias -8 (highlight protection), contrast -1 (tone),
  CA 15/20 (StudioRoom parity, Requirement 3.4). NOTE: firmware reloads lens_tune.tbl only on a
  lens change while AI is active — restart camera or toggle AI WB off/on to force reload.
- **Streak-clamp WB convergence change — WRITTEN IN SOURCE, NOT DEPLOYED.** `ai_lut.h`
  `ai_white_point_wb()` damping now widens the per-press gain step (base 160, +160 per
  consecutive same-direction press, cap 480) when successive presses point the same way (genuine
  persistent cast → converge in ~3 presses instead of never), reverting to the conservative
  base 160 when direction flips (noise safety preserved). Added file-scope streak state
  (`ai_wb_r_sign`/`ai_wb_r_streak`, same for b). Legit improvement for LiveView, but held from
  deployment because the real "green" cause turned out to be OVF timing, not convergence speed —
  this change does not fix OVF lag. Build + flash it only if/when LiveView AI WB convergence is
  the thing being tuned. Current card firmware does NOT include it.
- Relevant config state observed on card: `auto.ettr.ai.wb=1`, `auto.ettr.ai.pictune=1`,
  `auto.ettr.ai.wb.warmth=0`, `auto.ettr.ai.modes=1`.

## RaZStudio Consumer Contract (Summary)

- **Sidecar path:** `ML/DATA/SHOTS/{basename}.ml6d` (one per CR2)
- **Session path:** `ML/DATA/ml_export.json` (one per session)
- **Schema version:** `"2.0"` (major.minor string; major bump = breaking)
- **Always-present fields:** formatVersion, schema_version, filename, lens, pictureStyle, dualIsoPatch
- **Conditional fields:** dualIso (when active), ettr (when ETTR enabled)
- **Degradation:** Missing/broken contract files → default processing, never crash

## Important Constraints

- Firmware NEVER blocks or delays capture for metadata export
- All write failures are silent or non-blocking
- No floating point on code paths shared with Lua
- `autoexec.bin` and `.sym` must always be deployed together (matched pair)
- The `ai-lut-integration` branch is the working branch for these changes

## Validation Commands

```powershell
# Validate sidecars on pulled card:
python source-dev\build_tools\validate_sidecar.py D:\

# Inspect sidecar manually:
type D:\ML\DATA\SHOTS\IMG_XXXX.ml6d | python -m json.tool

# Check CR2 metadata with exiftool:
exiftool IMG_XXXX.CR2 | grep -E "ISO|White Balance|Red Balance|Blue Balance"

# MakerNote deep inspection:
exiftool -v3 -canon IMG_XXXX.CR2
```
