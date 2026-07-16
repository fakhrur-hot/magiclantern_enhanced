# AI-Assisted Exposure and Color for Magic Lantern

An end-to-end AI-assisted exposure and color control pipeline for Canon DSLRs
running Magic Lantern. It combines on-camera Lua scripting, offline model
training in Google Colab, a unified LUT format, and CI/CD automation to deliver
optimized ETTR, ALO, HTP, ISO, and WB decisions before each CR2 RAW capture.

The system is built around the constraints of the EOS 6D's DIGIC 5+ ARM
Cortex-R4 processor: **no on-camera ML inference, integer-only math, small
memory footprint, and SD card I/O for model storage.** All intelligence is
computed offline and compressed into a compact `unified.tbl` file. On-camera
Lua scripts perform only a table lookup and integer arithmetic -- no floating
point, no network, no GPU.

## Repository Structure

| Directory | Contents |
|---|---|
| `lua_scripts/` | On-camera Lua: logger and decision engine |
| `colab/` | Offline training notebook and standalone training script |
| `models/` | `unified.tbl` seed LUT and derived models |
| `logs/` | Sample logs (raw SD card logs are git-ignored) |
| `docs/` | Validation, troubleshooting, CI/CD documentation |
| `release_templates/` | Release notes templates |
| `.github/workflows/` | CI/CD build and release workflows |

## Quick Links

- [INSTALL.md](INSTALL.md) -- installation and SD card setup
- [ARCHITECTURE.md](ARCHITECTURE.md) -- full data flow and design
- [CONTRIBUTING.md](CONTRIBUTING.md) -- contribution guidelines
- [.kiro/specs/ai-lut-magic-lantern/](.kiro/specs/ai-lut-magic-lantern/) -- requirements, design, and task plan

## Status

Under active development on the `ai-lut-integration` branch. See the
[task plan](.kiro/specs/ai-lut-magic-lantern/tasks.md) for progress.

## Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md) -- full data flow and invariants
- [docs/VALIDATION.md](docs/VALIDATION.md) -- exiftool checks, match-rate metrics
- [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) -- common failures
- [docs/CI_CD.md](docs/CI_CD.md) -- workflows, manual triggers, adding a camera

## Glossary

- **ARM Cortex-R4** -- the CPU in the EOS 6D DIGIC 5+ processor. Real-time,
  integer-optimized. Lua runs here with no GPU and no FPU.
- **Integer-only math** -- all on-camera arithmetic uses whole numbers: no `/`
  division with fractional results, no decimal constants. `math.floor` is used
  exactly once (the histogram percentile).
- **LUT (Lookup Table)** -- `unified.tbl`, a pipe-delimited text table mapping
  Scene + LightLevel to camera decisions. Read by Lua; never executed as a model.
- **ETTR (Expose To The Right)** -- shutter adjustment to push the histogram
  right without clipping. Encoded as integer shutter denominators (50/100/200).
- **ALO (Auto Lighting Optimizer)** -- Canon shadow-lift tone processing.
  `shadow_boost` / `shadow_lift` / `neutral`. Phase 2 stub on camera.
- **HTP (Highlight Tone Priority)** -- Canon highlight-protection tone feature.
  `priority_on` / `priority_off`. Phase 2 stub on camera.
- **90th-percentile bin** -- integer histogram statistic: the bin index where
  cumulative pixel count first reaches 90% of the total. The "near-highlight"
  ETTR signal; the shared definition of LightLevel (0-255).
- **WB multipliers** -- integer sensor-gain ratios with `G=100` as the
  reference. Applied directly via `set_wb(r,g,b)`; no Kelvin conversion on camera.
- **Decision Tree** -- scikit-learn `DecisionTreeClassifier(max_depth=4)` trained
  offline to predict ISO from LightLevel. Its output is baked into `unified.tbl`.
- **Nearest-neighbor fallback** -- when no exact Scene+LightLevel row exists, the
  engine picks the same-scene row with the smallest integer `abs` light-level
  difference; then `unknown`; then safe defaults.
- **Hot-swap** -- replacing `unified.tbl` on the SD card takes effect on the next
  half-press, no reflash or reboot (the LUT is reloaded every half-press).

## License

Released under the [MIT License](LICENSE).
