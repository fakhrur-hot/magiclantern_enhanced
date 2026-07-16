# Architecture

<!-- TODO (Task 9): full data-flow documentation. Skeleton created in Task 1.
     Authoritative design lives in .kiro/specs/ai-lut-magic-lantern/design.md. -->

## Overview

Four layers connected by a continuous improvement loop:

1. **Camera layer (EOS 6D)** -- integer-only Lua: `unified_logger.lua` and
   `decision_engine.lua`.
2. **Offline training layer (Google Colab)** -- Pandas + scikit-learn produce
   `unified.tbl`.
3. **CI/CD layer (GitHub Actions)** -- `build.yml` and `release.yml`.
4. **Continuous improvement loop** -- new logs feed retraining.

## Data Flow

```
Camera half-press -> log -> SD card -> Colab train -> unified.tbl
-> SD card copy + git push -> CI build + release
-> decision_engine reads LUT -> applies integer decisions
-> new logs collected -> cycle repeats
```

See [.kiro/specs/ai-lut-magic-lantern/design.md](.kiro/specs/ai-lut-magic-lantern/design.md)
for the complete component design.
