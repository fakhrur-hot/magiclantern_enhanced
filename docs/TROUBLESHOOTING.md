# Troubleshooting

Common failures and how to diagnose them (REQ-007).

## Debug logging

Both Lua scripts append diagnostics to `A:/ML/logs/unified_log.txt`. For extra
detail, add debug lines to `A:/ML/logs/debug.txt` from a verbose branch in the
scripts. Marker entries to look for:

| Marker | Meaning |
|---|---|
| `LUT_NOT_FOUND` | `unified.tbl` was missing/unreadable; fallback used |
| `FALLBACK_USED` | no LUT row matched scene+light; safe defaults applied |
| `HIST_NIL` | `get_histogram()` returned nil; LightLevel 128 fallback |
| `WB_PARSE_ERROR` | WB string failed to parse; neutral WB applied |
| `ISO_PARSE_ERROR` | ISO string was nil/zero; ISO 400 applied |

## LUT not loaded

**Symptom:** every frame uses fallback defaults; `LUT_NOT_FOUND` in the log.

- Confirm the file is at exactly `A:/ML/models/unified.tbl` (path is
  case/drive sensitive).
- Confirm it is UTF-8 **without BOM** and uses `|`-delimited rows.
- Confirm the SD card is writable and the ML Lua module is enabled.

## Wrong ISO / WB in the CR2

**Symptom:** `exiftool` shows values that differ from the logged `Decision_*`.

- Check for `WB_PARSE_ERROR` / `ISO_PARSE_ERROR` in the log -- a malformed LUT
  row triggers the neutral/400 fallback.
- Verify the LUT row for that scene+light has 7 pipe-separated columns and a
  `R{n},G{n},B{n}` WB with `G=100`.
- If values are consistently ignored, the on-camera `set_iso` / `set_wb` /
  `set_shutter_fraction` API names may differ on your firmware -- reconcile them
  during on-camera testing (Task 10). The spec API names are the starting point,
  not a guarantee for every model.

## Camera reboots / crashes at ML load

- A Lua script over ~8 KB can crash Magic Lantern at boot (FIO_ReadFile assert).
  Keep each script small; both shipped scripts are well under the limit.
- Test script changes on a **spare** SD card first.

## Build failures (CI)

- **One camera fails, others succeed:** expected behavior -- `make || true` keeps
  the matrix alive and the release is tagged `[PARTIAL]` listing the missing
  camera. Inspect that job's log for the compile error.
- **All cameras fail:** usually a toolchain or path issue. Confirm
  `gcc-arm-none-eabi` installed and that `ML_SRC_ROOT` in `build.yml` points at
  the directory containing `platform/<camera>/`.
- **No release created:** `release.yml` only runs when `build.yml` concludes
  `success`. Check the build workflow's conclusion and that the workflow name
  matches the `workflow_run.workflows` entry exactly.

See `CI_CD.md` for workflow structure and how to add a camera to the matrix.
