# Installation

<!-- TODO: full installation steps. Skeleton created in Task 1. -->

## Prerequisites

- Canon EOS 6D (or other supported model) running Magic Lantern
- An SD card configured for Magic Lantern
- `exiftool` for CR2 metadata validation (see `docs/VALIDATION.md`)

## SD Card Layout

The on-camera scripts expect the following layout on the SD card (drive `A:`):

```
A:/ML/scripts/   -- unified_logger.lua, decision_engine.lua
A:/ML/models/    -- unified.tbl
A:/ML/logs/      -- unified_log.txt (created at runtime)
```

## Steps

1. Copy `lua_scripts/*.lua` to `A:/ML/scripts/`.
2. Copy `models/unified.tbl` to `A:/ML/models/`.
3. Enable the Lua module in Magic Lantern.
4. Half-press the shutter to trigger logging / decisions.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full data flow.
