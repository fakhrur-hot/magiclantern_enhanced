# CI/CD

Automated build and release pipeline (REQ-005). Two workflows in
`.github/workflows/`.

## Workflow diagram

```
push / PR to ai-lut-integration        nightly (0 2 * * *)
              \                        /
               v                      v
        +------------------------------------+
        |  build.yml  "Build AI Magic Lantern"|
        |  matrix: 6D.116 5D3.113 60D.111 650D.104
        |   - install gcc-arm-none-eabi       |
        |   - make clean && make || true      |
        |   - upload magiclantern-<camera>    |
        |  package-lut job:                   |
        |   - upload unified-lut (unified.tbl)|
        +------------------------------------+
                        | workflow_run: completed (success)
                        v
        +------------------------------------+
        |  release.yml                        |
        |   - download all artifacts          |
        |   - organize release/EOS-<model>/   |
        |   - copy unified.tbl into each      |
        |   - [PARTIAL] if any bin missing    |
        |   - gh release v<run_number>        |
        +------------------------------------+
```

## Triggers

- **build.yml:** push and PR to `ai-lut-integration`, plus nightly at 02:00 UTC.
- **release.yml:** on successful completion of `build.yml`
  (matched by the workflow name `Build AI Magic Lantern`).

## Manual trigger

The workflows currently trigger on push/PR/schedule. To run on demand, either
push a commit to `ai-lut-integration` or add a `workflow_dispatch:` trigger to
`build.yml`:

```yaml
on:
  workflow_dispatch:
  push:
    branches: [ai-lut-integration]
  # ...
```

Then use **Actions -> Build AI Magic Lantern -> Run workflow**.

## Add a new camera to the matrix

1. Confirm the camera's platform dir exists at
   `<ML_SRC_ROOT>/platform/<MODEL.VERSION>/` (e.g. `700D.115`).
2. Add the id to the matrix in `build.yml`:
   ```yaml
   matrix:
     camera: [6D.116, 5D3.113, 60D.111, 650D.104, 700D.115]
   ```
3. Add the same id to the `for cam in ...` loop in `release.yml`'s organize step
   so its binary is placed and its absence is detected.

## Operational gotchas (observed)

- **`workflow_run` and `schedule` only fire from the default branch.** GitHub
  runs these triggers using the workflow file on the repo's **default branch**
  (`main`), not on feature branches. Because `build.yml`/`release.yml` currently
  live only on `ai-lut-integration`, the nightly schedule and the automatic
  `release.yml` trigger will **not** run until these files are merged to `main`.
  The `push`/`pull_request` triggers do work on `ai-lut-integration` (that is
  what builds on every push). Merge to `main` to activate nightly + release.
- **Firmware may not build in stock CI yet.** A real Magic Lantern build needs
  more than `gcc-arm-none-eabi` + `cd platform/<cam> && make` (host tools, build
  invoked from the ML source root, etc.). Until that is wired up, build jobs stay
  green (`make || true`) but produce no `magiclantern.bin`, so releases would be
  `[PARTIAL]` with all cameras listed as missing. The `unified-lut` artifact is
  produced normally. Hardening the firmware build is separate from this pipeline.

## Notes

- `ML_SRC_ROOT` (env in `build.yml`) points at the vendored ML source. The spec
  references the build path as `platform/<camera>/`; in this repo the source is
  under `source-dev/`, so `ML_SRC_ROOT: source-dev`. Set it to `.` once the ML
  source is promoted to the repo root.
- Artifacts: `magiclantern-<camera>` (per camera) and `unified-lut` (once).
- `unified-lut` is uploaded by a dedicated `package-lut` job because
  `upload-artifact@v4` artifact names must be unique per run.
