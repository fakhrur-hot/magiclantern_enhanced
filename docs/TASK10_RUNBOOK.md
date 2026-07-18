# Task 10 -- Integration Validation Runbook

Step-by-step on-camera validation for the AI-LUT pipeline (REQ-006, REQ-007).
This requires **hardware in the loop** (EOS 6D + SD card + shutter releases) and
cannot be run in CI. Use a **spare** SD card first.

Companion tools:
- `tools/analyze_log.py` -- summarizes an exported log; flags markers, bad
  LightLevel, parse errors.
- `docs/VALIDATION.md` -- exiftool commands and the metric definitions.

---

## A. Logger-only pass (verify LightLevel)

1. Copy `lua_scripts/unified_logger.lua` to `A:/ML/scripts/` and the seed
   `models/unified.tbl` to `A:/ML/models/`.
2. Enable the ML Lua module; confirm the camera boots (no FIO_ReadFile assert).
3. Capture **10 half-press events** across a range of brightness (bright sky,
   normal, dim room, near-dark).
4. Export `A:/ML/logs/unified_log.txt` and run:
   ```sh
   python tools/analyze_log.py unified_log.txt
   ```
   - Expect 10 logger records, no markers, `RESULT: OK`.
   - Sanity-check that brighter scenes produced higher `LightLevel` (0-255) and
     darker scenes lower -- the values should track visual brightness.
5. **[ ] Pass criteria:** log populated, all LightLevel integers in 0-255,
   ordering matches scene brightness.

---

## B. Decision-engine pass (verify CR2 metadata)

1. Copy `lua_scripts/decision_engine.lua` to `A:/ML/scripts/` (keep the seed
   `unified.tbl` in `A:/ML/models/`).
2. Capture shots in **daylight, tungsten, shade, lowlight**.
3. For each CR2, read metadata (see `VALIDATION.md`):
   ```sh
   exiftool IMG_XXXX.CR2 | grep -E "ISO|Shutter|White Balance|Red Balance|Blue Balance"
   ```
4. Compare against the `Decision_*` lines the engine logged for that frame.
5. **[ ] Pass criteria:** ISO and WB in the CR2 exactly match the logged
   decision; after `reduce_shutter`, the 90th-percentile bin lands in 180-240.

### Fallback / error checks
- Remove `unified.tbl`; capture one frame; expect fallback (ISO 400, neutral WB,
  keep_shutter) + `LUT_NOT_FOUND` and `FALLBACK_USED` in the log.
- Confirm a malformed WB row yields neutral `set_wb(100,100,100)` + a
  `WB_PARSE_ERROR|raw=...|fallback=R100,G100,B100` entry.

---

## C. Retrain + hot-swap pass

1. Export the accumulated `unified_log.txt` to a host machine.
2. Produce a retrained `unified.tbl` from it with the offline training.
3. Copy the new `unified.tbl` onto `A:/ML/models/` **without rebooting**.
4. Capture a second round; confirm the **new** decisions take effect on the next
   half-press (hot-swap -- the LUT is reloaded every event).
5. **[ ] Pass criteria:** new decisions differ as expected and apply with no
   reflash/reboot.

---

## D. Record results

Fill in `docs/VALIDATION.md` section 4 with:

| Round | ISO match rate | WB match rate | ETTR histogram (bin in 180-240) |
|-------|----------------|---------------|---------------------------------|
| Seed LUT      |  %  |  %  |  /  frames |
| Retrained LUT |  %  |  %  |  /  frames |

---

## E. Fix Lua API mismatches (expected)

The on-camera getters/setters use the spec's API names. Real firmware may differ.
Watch for and reconcile:

- `get_histogram()` shape (table indexing, bin count) -> adjust
  `compute_light_level` / `get_histogram_string` if needed.
- `camera.iso.value`, `camera.shutter.value`, `dryos.date` field spellings
  (`minute`/`min`, `second`/`sec`) -> the getters already try both.
- `set_shutter_fraction`, `set_iso`, `set_wb` -> confirm names/signatures; the
  decision engine isolates each in its own `apply_*` function for easy patching.
- If a scene source becomes available on camera, replace the `get_scene()`
  `"unknown"` stub so the training loop can learn per-scene WB.

Keep each script **< 8 KB** after edits (ML boot-time size crash threshold).
Re-run section A/B after any Lua change.
