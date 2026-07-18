# AI-Assisted Exposure and Color for Magic Lantern (EOS 6D)

A Magic Lantern firmware extension that makes ML's **own** exposure and color
controls smarter, driven by a compact lookup table learned from your own
shooting. It improves ETTR, ISO, white balance, ALO/HTP and per-lens picture
tuning — with a **single source of adjustment per feature**, all through ML's
existing menu.

Built for the EOS 6D's DIGIC 5+ **ARM Cortex-R4**: no FPU, no GPU, no on-camera
ML inference. All on-camera logic is **integer-only C** inside the ETTR module.
The learned model is compressed into a small `unified.tbl` on the SD card; the
firmware only does a table lookup and integer arithmetic. Model training happens
offline and is not part of the on-camera code.

## Features (Expo → Auto ETTR)

- **Auto ISO Optimizer** — when Canon Auto ISO is engaged, the AI applies the
  learned ISO floor + HTP/ALO and (in **M**) hands the exposure to the real
  metered ETTR. Exposure target defaults to −0.5 EV (highlights just under clip).
- **AI Data Logging** — appends sensor stats to `A:/ML/logs/unified_log.txt` on
  each half-press (RAW-histogram light level, per-channel percentiles,
  ISO/shutter/WB, file number) for offline training.
- **AI White Balance** — confidence-clipped bright-pixels auto-WB from the RAW
  channels; neutralizes real white highlights without forcing dim ones. Affects
  RAW (as-shot) + JPEG.
- **AI Picture Tune** — per-lens Canon contrast/saturation from
  `ML/models/lens_tune.tbl`, for a uniform look across lenses. JPEG only.
- **AI Modes** — restricts the whole system to **P, M** or **P, M, Av, Tv**. In
  Av your aperture is never overwritten; in Tv your shutter is never overwritten;
  in M the AI has full control.

## How it works

```
camera (ettr.mo): half-press -> log sensor stats  -> unified_log.txt
                                read unified.tbl   -> drive ML's ETTR/ISO/WB/ALO/HTP
        |  export unified_log.txt
        v
offline training (internal)   -> new unified.tbl
        |  copy to A:/ML/models/  (hot-swap; next half-press uses it)
        v
back on camera -> new logs -> repeat
```

The LUT hot-swaps: drop a new `unified.tbl` onto the card and it takes effect on
the very next half-press — no reflash, no reboot.

## Install

- Grab a release (or a locally built `ai-magiclantern-<camera>-full.zip`) and
  extract it onto the SD card; the `ML/` tree contains the module + `unified.tbl`
  + `lens_tune.tbl` and creates `ML/logs/` for the log.
- Already running Magic Lantern? Copy the AI-integrated `ML/modules/ettr.mo` and
  `ML/models/*.tbl` onto the card. See [INSTALL.md](INSTALL.md).

## Repository structure

| Path | Contents |
|---|---|
| `source-dev/` | Vendored Magic Lantern source; the AI lives in `modules/ettr/` (`ai_lut.h` + `ettr.c`) |
| `models/` | `unified.tbl` (learned LUT) and `lens_tune.tbl` (per-lens picture tune) |
| `tools/` | `validate_exposure.py`, `analyze_log.py`, `make_full_bundle.py` |
| `docs/` | Validation, troubleshooting, CI/CD, field logging, ETTR-AI integration |
| `.github/workflows/` | CI build + release |
| `dist/` | Built installers (git-ignored) |

## Documentation

- [INSTALL.md](INSTALL.md) — SD card setup
- [ARCHITECTURE.md](ARCHITECTURE.md) — data flow and invariants
- [docs/ETTR_AI_INTEGRATION.md](docs/ETTR_AI_INTEGRATION.md) — how the AI drives ML's ETTR
- [docs/FIELD_LOGGING_CHECKLIST.md](docs/FIELD_LOGGING_CHECKLIST.md) — collecting training data
- [docs/VALIDATION.md](docs/VALIDATION.md) · [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) · [docs/CI_CD.md](docs/CI_CD.md)

## Glossary

- **ETTR (Expose To The Right)** — push highlights just below clipping for
  maximum signal. The real metered ETTR owns the exposure push; the AI supplies
  learned starting points (ISO/WB/ALO/HTP).
- **unified.tbl** — the LUT: pipe-delimited `Scene|LightLevel|ETTR|ALO|HTP|ISO|WB`.
  Read by the firmware; never executed as a model. Capped at ≤128 rows / <8 KB.
- **LightLevel** — integer 0–255 from the 90th-percentile of the **RAW** green
  histogram (true sensor exposure, not the ExpSim preview).
- **ALO / HTP** — Canon Auto Lighting Optimizer / Highlight Tone Priority, driven
  via ML's `set_alo` / `set_htp`.
- **White-point WB** — bright-pixels + gray-world estimate, confidence-blended and
  temporally damped, applied as custom WB gains (AsShotNeutral scale, 1024=neutral).
- **Hot-swap** — replacing `unified.tbl` on the card takes effect on the next
  half-press; no reflash or reboot.
- **Single source of adjustment** — each feature (ETTR/ISO/WB/ALO/HTP) is driven
  from exactly one place (the ETTR module) through ML's own controls.

## License

Released under the [MIT License](LICENSE).
