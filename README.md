# AI-Assisted Exposure and Color for Magic Lantern

A Magic Lantern firmware extension that makes ML's **own** exposure and color
controls smarter, driven by a compact lookup table learned from your own
shooting. It improves ETTR, ISO, white balance, ALO/HTP and per-lens picture
tuning — with a **single source of adjustment per feature**, all through ML's
existing menu.

Built for the DIGIC 5 / DIGIC 5+ **ARM Cortex-R4** cores in Magic Lantern
cameras: no FPU dependency for the AI path, no GPU, no on-camera ML inference.
It supports everything original Magic Lantern supports. All on-camera AI logic
is **integer-only C** inside the ETTR module. The learned model is compressed
into a small `unified.tbl` on the SD card; the firmware only does a table
lookup and integer arithmetic. Model training happens offline and is not part
of the on-camera code.

## Supported cameras

This fork targets **DIGIC 5 and DIGIC 5+** bodies only (classification per the
[Magic Lantern DIGIC wiki](https://wiki.magiclantern.fm/digic)) — the AI
features are written and tuned for that hardware generation, and are
**intentionally not built for DIGIC 4 or earlier** (e.g. 60D).

| Camera | Platform dir | DIGIC | Hardware-validated |
|---|---|---|---|
| **EOS 6D** | `6D.116` | 5+ | ✅ Primary target — built and field-tested every session |
| EOS 5D Mark III | `5D3.113`, `5D3.123` | 5+ | ⚠️ Compiles clean; not run on hardware |
| EOS 70D | `70D.112` | 5+ | ⚠️ Compiles clean; not run on hardware |
| EOS 650D / Rebel T4i | `650D.104` | 5 | ⚠️ Compiles clean; not run on hardware |
| EOS 700D / Rebel T5i | `700D.115` | 5 | ⚠️ Compiles clean; not run on hardware |
| EOS 100D / Rebel SL1 | `100D.101` | 5 | ⚠️ Compiles clean; not run on hardware |
| EOS M | `EOSM.202` | 5 | ⚠️ Compiles clean; not run on hardware |

**Feature availability is guarded per camera's actual capability, not assumed
uniform:**
- The AI-LUT/ETTR/WB engine (`ai_lut.h`) only touches generic ML core APIs
  (`raw_*`, `lens_*`, `picstyle_*`) that are already ported per-camera by
  upstream ML; where a capability is a stub on a given body (e.g. no raw
  LiveView request), the existing `(void*)&raw_lv_request != (void*)&ret_0`-style
  checks already in `ettr.c` skip that path rather than call a no-op.
- `sd_uhs` (SD overclocking) has its own **explicit per-body register table**
  (`sd_uhs.c`); a camera not in that table silently no-ops instead of writing
  registers blind. All 7 bodies above are in the table.
- Only **6D** has had any of this actually run on real hardware this project
  cycle. Treat the other 6 as "should work, unverified" — please report back
  if you test one.

## Features (Expo → Auto ETTR)

- **Auto ISO Optimizer** — when Canon Auto ISO is engaged, the AI applies the
  learned ISO floor + HTP/ALO and (in **M**) hands the exposure to the real
  metered ETTR. Exposure target defaults to −0.5 EV (highlights just under clip).
- **AI Data Logging** — appends sensor stats to `A:/ML/logs/unified_log.txt` on
  each half-press (RAW-histogram light level, per-channel percentiles,
  ISO/shutter/WB, file number) for offline training.
- **AI White Balance** — confidence-clipped bright-pixels auto-WB from the RAW
  channels. Neutralizes real white highlights without forcing dim ones; a
  CLAHE-style shadow clip keeps sensor-noise ratios in the dark from ever
  driving a color cast (deep shadows glide to the sensor's daylight prior, so
  no blue tint can survive). Runs in both LiveView and, for OVF shooters, from
  the just-taken photo during image review. Affects RAW (as-shot) + JPEG.
- **AI WB Warm/Cool** — the neutral gains carry the science; a separate,
  user-chosen bias rides **Canon's own WB Shift (Amber/Blue axis, ~5 mireds per
  step)**, exactly how Canon separates White Priority from Ambience Priority.
  9 options from **B4 cool** through **Neutral** (default) to **A4 warm** —
  pick a direction, it's no longer warm-only. Genuinely golden light (well
  past daylight) still auto-adds up to +1 amber on top of your choice so
  sunset/sunrise keeps some character, but it can't override a cool/neutral
  preference the way the old one-directional control could.
- **ETTR in P / Av / Tv** *(experimental, OVF path)* — Canon's own auto-exposure
  modes ignore direct shutter/ISO writes, so instead of fighting Canon, this
  biases its metering via exposure compensation, targeting highlights **0.5–0.7
  EV under clipping** (not right at it) and converging over a couple of shots.
  M mode keeps the original direct shutter/ISO ETTR.
- **AI Lens Tune** — an on-camera editor (modeled on Canon's per-lens AFMA
  memory) for each lens's row in `ML/models/lens_tune.tbl`: picture-style
  contrast/saturation/tone, a per-lens ETTR **exposure bias**, and per-lens WB
  trims. Values auto-load on every lens swap; **Save** writes the row (marked so
  offline training preserves your hand-tuning). Normalizes exposure and color
  across a mixed lens kit.
- **AI Modes** — restricts the whole system to **P, M** or **P, M, Av, Tv**. In
  Av your aperture is never overwritten; in Tv your shutter is never overwritten;
  in M the AI has full control.

## What's distinct from upstream Magic Lantern

This project is a fork of the **`magiclantern_simplified`** source (vendored in
`source-dev/`, and archived pristine as `magiclantern_simplified-dev.zip` for
reference). Everything upstream ML does still works — the files below are the
only ones that differ from that base. Diff any of them against the archived
source to see exactly what changed.

**New files (this fork only):**

| File | What it adds |
|---|---|
| `source-dev/modules/ettr/ai_lut.h` | The whole AI engine: AI-LUT scene lookup, confidence-clipped **AI White Balance** (with green-cast compensation calibrated against Canon's own measured WB), **AI WB Warm/Cool** bias, per-lens **AI Lens Tune**, extended ETTR metadata, and firmware **data logging**. |
| `source-dev/modules/ettr/gyro_bridge.{c,h}` | External-IMU-over-hot-shoe + electronic-level bridge and `mlc.gyro()` / `mlc.level()` Lua bindings for video metadata. **Experimental, currently compiled out** — see note below. |
| `source-dev/modules/ettr/mlv_metadata.{c,h}` | Per-frame GYRO/ETTR blocks embedded into `.MLV` recordings. Same experimental subsystem, currently compiled out. |
| `models/unified.tbl`, `models/lens_tune.tbl` | The learned scene LUT and the per-lens tuning table (contrast/sat/tone, EV bias, WB trims, CA/fringe). |
| `lua_scripts/mov_metadata.lua` | Companion Gyroflow/metadata logger script (experimental). |

**Modified upstream files:**

| File | Change vs upstream |
|---|---|
| `source-dev/modules/ettr/ettr.c` | Auto ISO Optimizer, all AI menu entries + hooks, per-frame highlight metering for **ETTR in P/Av/Tv** via exposure compensation, and firmware-side data logging. |
| `source-dev/modules/sd_uhs/sd_uhs.c` | Added an **Auto Speed Test** wizard (`Prefs → SD Overclock`) that steps 240→192→160 MHz across reboots, auto-skipping any preset the card rejects (Canon safe-mode register), plus 6D/70D read/write pause hooks. |
| `source-dev/modules/lua/{lua.c,lua_common.h,Makefile}` | Fixed the long-dormant `LUA_CBR_FUNC(vsync/…)` argument bug, enabled `CONFIG_VSYNC_EVENTS`, and exported the few Lua C API symbols other modules call cross-module. |

> **Video gyro / MLV-metadata is present but disabled.** The
> `gyro_bridge`/`mlv_metadata` subsystem polls the hot-shoe serial every
> LiveView frame, which conflicts with a mounted flash and destabilized ETTR
> in the field. It is compiled out (`ETTR_VIDEO_GYRO_METADATA 0` in `ettr.c`,
> and its objects are dropped from the module link) so core ETTR stays
> self-contained and robust. Re-enable only after the hot-shoe/flash conflict
> is resolved and validated on hardware.
>
> **A companion-app sidecar contract (`.ml6d` files + `ml_export.json`) was
> tried and retired.** The intended consumer app doesn't read per-shot
> sidecar files (it reads standard CR2 EXIF instead — LensID, ISO,
> ColorTemperature), and the writer was hitting an unresolved on-camera
> file-write failure besides. Removed entirely rather than keep dead,
> silently-failing code. The AI's actual corrections (WB, exposure, lens
> tune) already land in standard Canon EXIF/RAW metadata with no extra
> firmware plumbing needed.

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

- Grab the release zip matching your camera (`ai-magiclantern-<camera>.zip` —
  see [Supported cameras](#supported-cameras) for the exact platform name, e.g.
  `6D.116`, `5D3.113`) and extract it onto the SD card; the `ML/` tree contains
  the module + `unified.tbl` + `lens_tune.tbl` and creates `ML/logs/` for the log.
- Already running Magic Lantern? Copy the AI-integrated `ML/modules/ettr.mo` and
  `ML/models/*.tbl` onto the card. See [INSTALL.md](INSTALL.md).
- Building locally for a camera other than 6D? `cd source-dev/platform/<camera>
  && make` — every camera in the support table above builds from the same
  `source-dev/` tree with no per-camera source changes needed.

## Repository structure

| Path | Contents |
|---|---|
| `source-dev/` | Vendored `magiclantern_simplified` source, shared by all 7 supported cameras. Fork changes live in `modules/ettr/` (`ai_lut.h` + `ettr.c` + gyro/mlv), `modules/sd_uhs/` (Auto Speed Test) and `modules/lua/` (see the table above). Per-camera platform dirs (`platform/6D.116/`, `platform/5D3.113/`, etc.) are otherwise untouched. |
| `magiclantern_simplified-dev.zip` | Pristine original-developer source archive, kept for reference/diffing |
| `models/` | `unified.tbl` (learned LUT) and `lens_tune.tbl` (per-lens tune) |
| `tools/` | `validate_exposure.py`, `analyze_log.py`, `make_bundle.py`, `make_full_bundle.py` |
| `docs/` | Validation, troubleshooting, CI/CD, field logging, ETTR-AI integration |
| `.kiro/` | Kiro specs + steering context |
| `.github/workflows/` | CI build + release (matrix = every camera in the support table) |
| `dist/` | Built installers (git-ignored) |

## Documentation

- [INSTALL.md](INSTALL.md) — SD card setup
- [ARCHITECTURE.md](ARCHITECTURE.md) — data flow and invariants
- [docs/ETTR_AI_INTEGRATION.md](docs/ETTR_AI_INTEGRATION.md) — how the AI drives ML's ETTR
- [docs/FIELD_LOGGING_CHECKLIST.md](docs/FIELD_LOGGING_CHECKLIST.md) — collecting training data
- [docs/VALIDATION.md](docs/VALIDATION.md) · [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) · [docs/CI_CD.md](docs/CI_CD.md)
- [docs/RAZSTUDIO_CONTRACT.md](docs/RAZSTUDIO_CONTRACT.md) — **retired**; kept for history only (see the banner at its top)

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
