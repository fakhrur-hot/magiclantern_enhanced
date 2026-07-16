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

## Glossary

<!-- TODO (Task 9): full glossary of key terms (ETTR, ALO, HTP, LUT,
     90th-percentile bin, WB multipliers, hot-swap, etc.). -->

## License

Released under the [MIT License](LICENSE).
