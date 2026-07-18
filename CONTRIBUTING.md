# Contributing

Thanks for your interest in improving this project.

## Branching

- All AI feature work lives on the `ai-lut-integration` branch.
- **Never** push to `main` or `master`.
- The `ai-lut-integration` branch must never be force-pushed once CI is active.

## On-Camera Lua Rules (non-negotiable)

The DIGIC-era ARM cores in ML cameras have **no FPU**. On-camera Lua must be
integer-only:

- No floating-point constants (e.g. `0.9`).
- No `/` division that produces fractions.
- No `math.sqrt`, trigonometric calls, or `%.2f`-style float formatting.
- `math.floor` is permitted only where noted in the design (it returns an integer).
- Each Lua script must stay **under 10 KB** (oversized scripts crash ML at boot).

## Workflow

1. Create a topic branch off `ai-lut-integration`.
2. Make your change; keep commits focused.
3. Verify Lua scripts on a **spare** SD card before touching the primary card.
4. Open a PR targeting `ai-lut-integration`.

## Commit Messages

Use conventional prefixes: `feat:`, `fix:`, `docs:`, `chore:`, `ci:`.

## License

By contributing you agree that your contributions are licensed under the
[MIT License](LICENSE).
