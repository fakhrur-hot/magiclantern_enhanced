# Troubleshooting

Common failures and how to diagnose them. All on-camera AI behavior lives in
the firmware ETTR module (`ML/modules/ettr.mo`); there are no on-camera Lua
scripts to debug.

## Where to look first

- `A:/ML/logs/unified_log.txt` — one record per half-press when **AI Data
  Logging** is on. A healthy record has a real `LightLevel` (not 128 with an
  empty Hist), a resolved `ISO`, and `RawMed/RawP99/RawR/RawB` values.
- ML menu → Expo → **Auto ETTR** — every AI toggle is here: Auto ISO Optimizer,
  AI Modes, AI Data Logging, AI White Balance, AI Picture Tune.

## LUT not applied

**Symptom:** ISO/HTP/ALO never change from Canon defaults when Auto ISO engages.

- Confirm the file is at exactly `A:/ML/models/unified.tbl`.
- Keep it **< 8 KB and ≤ 128 data rows** — the firmware reads it with a single
  8 KB read and drops the rest. (`tools/make_full_bundle.py` refuses to bundle
  an oversized table.)
- Rows with out-of-range values (LightLevel outside 0–255, ISO outside
  50–25600, WB gains outside 10–400) are silently skipped as corrupt.
- Check **AI Modes**: outside the covered shooting modes (P, M by default) the
  whole AI system deliberately does nothing.
- The LUT is reloaded on each engage — replacing the file on the card takes
  effect on the next half-press, no reboot needed.

## No log records / LightLevel stuck at fallback

**Symptom:** `unified_log.txt` missing records, or `LightLevel=128` with
`HistSrc=disp`.

- Be in **LiveView** when half-pressing — RAW metering needs the LV raw buffer.
- Shoot **full RAW**, not mRAW/sRAW — reduced-raw modes break raw metering.
- Raw detection can *transiently* fail on some bodies depending on the lens/AF
  state at power-on ("raw detect error" — a known ML quirk; forum reports show
  toggling the lens MF→AF or re-entering LiveView recovers it). The firmware
  already retries raw metering across polls / for ~0.5 s after a shot, so an
  occasional missed record is expected recovery, not a fault.

## ETTR misses / "hit and miss" exposure

- **OVF shooting is reactive:** ETTR meters the shot just taken (during image
  review) and corrects the *next* one. Enable Canon **image review** or the
  metering pass never runs. In LiveView M, half-press meters *before* the shot.
- **Dark scenes:** if the camera is already at max Auto ISO / slowest shutter,
  there is nothing left to push — raise the Auto ISO ceiling.
- Verify with `tools/validate_exposure.py <unified_log.txt> <cr2_dir>`: it
  reports per-shot clip% / headroom EV and a verdict (CLIPPED / UNDER / ETTR-OK).

## Green or wrong-looking AI White Balance

- AI WB needs usable highlights; in scenes with clipped highlights confidence
  is halved and the correction is damped — a slow drift toward neutral is by
  design, not a stuck value.
- WB gains are AsShotNeutral-style (1024 = neutral, *higher* gain suppresses
  that channel). If you post-process, check the CR2's AsShotNeutral rather than
  eyeballing the embedded JPEG preview.

## LiveView acts up after settings changes

- Programmatic changes to LV-affecting settings (ALO/HTP/WB pushes included)
  can, rarely, wedge LiveView — a long-standing ML quirk, not specific to this
  fork. The community workaround: **press MENU twice** (a Canon menu
  round-trip re-inits LV). If LV stays wedged, power-cycle.
- If the ML overlays flicker, make sure you are on a current build of this
  fork's `ettr.mo` — older builds redrew the GUI every poll while logging.

## Camera hangs at boot (stuck after sensor cleaning, battery pull needed)

**Cause:** `autoexec.bin` and `ML/modules/<camera>.sym` are a **matched pair** —
the sym file maps module imports to that exact core build's addresses. Updating
one without the other sends module relocations to garbage addresses and the
camera hard-hangs at module load (too early for a crash log).

- Always copy **both** files when updating the core; the release zip contains
  the matching set. Extracting the whole zip is always safe.
- To recover: pull the battery, put the card in a reader, restore the previous
  `autoexec.bin` **and** its matching sym together.
- Swapping only a module (`ettr.mo`) is safe — modules resolve by name against
  whatever core the card carries.

## Camera reboots / crashes at ML load

- Any file ML slurps in one read must stay small: `unified.tbl` /
  `lens_tune.tbl` under 8 KB. A Lua script over ~8 KB also crashes ML at boot
  (FIO_ReadFile assert) — this fork ships none, but if you add your own, keep
  them small.
- Test changes on a **spare** SD card first.
- If you use the **sd_uhs** (SD overclock) module: some cards get *slower* or
  stop writing entirely with the hack enabled — disable it when diagnosing
  write failures or missing logs.

## Build failures (CI)

- **One camera fails, others succeed:** expected — `make || true` keeps the
  matrix alive and the release is tagged `[PARTIAL]` listing the missing
  camera. Inspect that job's log for the compile error.
- **All cameras fail:** usually a toolchain or path issue. Confirm
  `gcc-arm-none-eabi` installed and that `ML_SRC_ROOT` in `build.yml` points at
  the directory containing `platform/<camera>/`.
- **No release created:** `release.yml` only runs when `build.yml` concludes
  `success`. Check the build workflow's conclusion and that the workflow name
  matches the `workflow_run.workflows` entry exactly.

See `CI_CD.md` for workflow structure and how to add a camera to the matrix.
