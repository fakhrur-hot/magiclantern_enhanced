# Design Document: ALO/HTP Scene Optimizer

## Overview

The ALO/HTP Scene Optimizer replaces the existing `ai_apply_alo_htp()` function with a new integer-only implementation that classifies scenes into three brightness zones (Dark, Normal, Bright) using raw histogram percentiles (P90, P99) and applies zone-appropriate ALO level, HTP state, and a minimal ±1/3 EV ISO nudge. In P mode, when ISO is already at the floor in Bright_Zone, a shutter-speed fallback provides highlight protection instead.

## Architecture

The Scene Optimizer replaces the existing `ai_apply_alo_htp()` function in `source-dev/modules/ettr/ettr.c`. It is a single static C function (`static void ai_apply_alo_htp(void)`) that takes no parameters, reads scene data from module-global state, classifies the scene into one of three brightness zones, and drives four outputs: ALO level, HTP state, ISO nudge, and (in P mode only) shutter fallback.

The function is invoked from `auto_iso_optimizer_step()` on every polling tick where `auto_iso_optimizer` is enabled and a half-shutter press is active. It executes at most once per half-press via the existing `ai_logged_press` guard pattern.

### Data Flow

```
┌─────────────────────┐
│  ai_light_level()   │──→ P90 (return value, 0–255)
│  (raw histogram)    │──→ P99 (side-effect: ai_raw_p99, 0–255)
└─────────────────────┘
           │
           ▼
┌─────────────────────┐      ┌───────────────────────┐
│  Zone Classification │      │  Globals Read         │
│  ─────────────────── │      │  ───────────────────  │
│  P90 < 80 → Dark    │      │  lens_info.raw_iso    │
│  P99 ≥ 240 → Bright │      │  lens_info.raw_shutter│
│  else → Normal       │      │  shooting_mode        │
└─────────────────────┘      │  get_htp(), get_alo() │
           │                   └───────────────────────┘
           ▼                              │
┌─────────────────────────────────────────┘
│  Decision Logic (integer-only)
│  ─────────────────────────────
│  1. Compute desired ALO, HTP from zone
│  2. Compute desired ISO from zone + current raw_iso
│  3. Compute desired shutter (P mode + Bright + ISO floor only)
│  4. Apply setter minimization (skip unchanged values)
│  5. Write: set_htp() → set_alo() → lens_set_rawiso() → lens_set_rawshutter()
└──────────────────────────────────────────────────────────────────────────────
```

## Components and Interfaces

### Components

### 1. Zone Classifier

Determines scene brightness zone from P90 and P99 values.

```c
typedef enum { ZONE_DARK, ZONE_NORMAL, ZONE_BRIGHT } scene_zone_t;

static scene_zone_t classify_zone(int p90, int p99)
{
    if (p99 >= 240) return ZONE_BRIGHT;   /* highlight clipping takes priority */
    if (p90 < 80)   return ZONE_DARK;
    return ZONE_NORMAL;
}
```

**Design decision:** Bright_Zone check comes first. A scene can have both low P90 (dark midtones) and high P99 (specular highlights). Highlight protection is the priority because blown highlights are unrecoverable, whereas shadows can be lifted in post.

### 2. ALO Level Resolver

Maps zone + P90 sub-range to the desired ALO level.

```c
static int resolve_alo(scene_zone_t zone, int p90)
{
    switch (zone) {
        case ZONE_DARK:
            if (p90 < 25) return ALO_HIGH;
            if (p90 < 50) return ALO_STD;
            return ALO_LOW;           /* P90 in [50, 79] */
        case ZONE_BRIGHT:
            return ALO_LOW;
        case ZONE_NORMAL:
        default:
            return ALO_STD;
    }
}
```

### 3. HTP Resolver

```c
static int resolve_htp(scene_zone_t zone)
{
    /* Dark → OFF (avoid ISO floor penalty); Normal/Bright → ON */
    return (zone != ZONE_DARK) ? 1 : 0;
}
```

### 4. Exposure Nudge Logic

Computes desired raw_iso and raw_shutter based on zone, current values, and shooting mode.

```c
/* Returns 1 if shutter was adjusted instead of ISO (fallback path) */
static int compute_nudge(scene_zone_t zone, int cur_iso, int cur_shutter,
                         int mode, int *out_iso, int *out_shutter)
{
    *out_iso = cur_iso;
    *out_shutter = cur_shutter;

    switch (zone) {
        case ZONE_DARK:
            *out_iso = cur_iso + 8;    /* +1/3 EV */
            return 0;

        case ZONE_BRIGHT:
            if (cur_iso > 72) {
                *out_iso = cur_iso - 8;  /* -1/3 EV */
            } else if (mode == SHOOTMODE_P) {
                *out_shutter = cur_shutter + 8;  /* 1/3 EV faster */
                return 1;
            }
            /* Av/Tv/M at floor: no change possible */
            return 0;

        case ZONE_NORMAL:
        default:
            return 0;  /* no exposure change */
    }
}
```

### 5. Setter Minimization & Apply

Writes hardware registers only when the computed value differs from the current state. Respects the ordering constraint: `set_htp()` before `set_alo()` (Canon firmware suppresses ALO when HTP is active, so HTP must be cleared first when transitioning from Bright/Normal to Dark).

```c
static void ai_apply_alo_htp(void)
{
    /* Feature gate */
    if (!auto_iso_optimizer) return;

    /* Scene analysis */
    int p90 = ai_light_level();
    int p99 = ai_raw_p99;
    if (p90 < 0) return;               /* histogram unavailable */

    /* Zone classification */
    scene_zone_t zone = classify_zone(p90, p99);

    /* Resolve desired settings */
    int want_htp = resolve_htp(zone);
    int want_alo = resolve_alo(zone, p90);

    /* Compute exposure nudge */
    int want_iso, want_shutter;
    compute_nudge(zone, lens_info.raw_iso, lens_info.raw_shutter,
                  shooting_mode, &want_iso, &want_shutter);

    /* Apply with setter minimization (HTP before ALO per Req 9.3) */
    if (want_htp != get_htp()) set_htp(want_htp);
    if (want_alo != get_alo()) set_alo(want_alo);
    if (want_iso != lens_info.raw_iso) lens_set_rawiso(want_iso);
    if (want_shutter != lens_info.raw_shutter) lens_set_rawshutter(want_shutter);
}
```

### Interfaces

### Input (Read)

| Symbol | Type | Description |
|--------|------|-------------|
| `ai_light_level()` | `int` (0–255 or -1) | Returns P90 green, populates `ai_raw_p99` |
| `ai_raw_p99` | `static int` | P99 green channel (0–255, -1 if unavailable) |
| `lens_info.raw_iso` | `int` | Current raw ISO (72 = ISO 100, +8 per 1/3 EV) |
| `lens_info.raw_shutter` | `int` | Current raw shutter (+8 = 1/3 EV faster) |
| `shooting_mode` | `int` | `SHOOTMODE_P`(0), `SHOOTMODE_TV`(1), `SHOOTMODE_AV`(2), `SHOOTMODE_M`(3) |
| `get_htp()` | `int` | Current HTP state (0=OFF, 1=ON) |
| `get_alo()` | `int` | Current ALO level (ALO_OFF, ALO_LOW, ALO_STD, ALO_HIGH) |
| `auto_iso_optimizer` | `static int` | Feature gate toggle |

### Output (Write)

| Symbol | Type | Description |
|--------|------|-------------|
| `set_htp(int)` | `void` | Set HTP ON/OFF |
| `set_alo(int)` | `void` | Set ALO level |
| `lens_set_rawiso(int)` | `int` | Set raw ISO value |
| `lens_set_rawshutter(int)` | `int` | Set raw shutter value |

### Constants

```c
#define ISO_RAW_FLOOR    72     /* ISO 100 in Canon raw units */
#define ISO_NUDGE_STEP    8     /* 1/3 EV in raw units */
#define SHUTTER_NUDGE_STEP 8   /* 1/3 EV in raw units */
#define DARK_THRESHOLD   80     /* P90 below this → Dark_Zone */
#define BRIGHT_THRESHOLD 240    /* P99 at or above this → Bright_Zone */
#define DARK_SEVERE      25     /* P90 below this → ALO High */
#define DARK_MODERATE    50     /* P90 below this → ALO Standard */
```

## Data Models

No new data structures are required. The function operates on existing module-scope globals and Canon property interfaces. The `scene_zone_t` enum is local to the function or file scope.

### Zone Classification Truth Table

| P90 | P99 | Zone | ALO | HTP | ISO Δ | Shutter Δ (P only) |
|-----|-----|------|-----|-----|-------|---------------------|
| [0, 24] | < 240 | Dark | High | OFF | +8 | 0 |
| [25, 49] | < 240 | Dark | Std | OFF | +8 | 0 |
| [50, 79] | < 240 | Dark | Low | OFF | +8 | 0 |
| ≥ 80 | < 240 | Normal | Std | ON | 0 | 0 |
| any | ≥ 240 | Bright | Low | ON | -8 (or 0 at floor) | +8 if ISO at floor |

### Mode × Zone Exposure Matrix

| Mode | Dark | Normal | Bright (ISO>72) | Bright (ISO==72) |
|------|------|--------|-----------------|------------------|
| P | ISO +8 | — | ISO -8 | Shutter +8 |
| Av | ISO +8 | — | ISO -8 | — (no change) |
| Tv | ISO +8 | — | ISO -8 | — (no change) |
| M | ISO +8 | — | ISO -8 | — (no change) |

## Error Handling

| Condition | Response |
|-----------|----------|
| `ai_light_level()` returns -1 | Early return, no settings modified |
| `ai_raw_p99` is -1 (unavailable) | Treat as < 240 (cannot be Bright_Zone without valid P99) |
| `lens_info.raw_iso` is 0 (Auto ISO active) | Skip ISO nudge (0 ± 8 is meaningless); ALO/HTP still apply |
| Feature gate off (`auto_iso_optimizer == 0`) | Early return, no execution |

## Integration Points

### Caller: `auto_iso_optimizer_step()`

The new `ai_apply_alo_htp()` is called from the existing `auto_iso_optimizer_step()` function in `ettr.c`. The call replaces the current invocation that passes `(scene_dr, highlight_headroom)` float parameters. The new function takes no parameters (reads globals directly).

### Trigger Guard

The `ai_logged_press` pattern ensures the function fires at most once per half-shutter cycle:
- Set to 0 when half-shutter is released
- Set to 1 after first execution in a press cycle
- The optimizer checks this guard before calling `ai_apply_alo_htp()`

### Existing Function Removal

The current `ai_apply_alo_htp(float scene_dr, float highlight_headroom)` (lines ~267–280 of ettr.c) is replaced entirely. The `#define` constants `AI_HTP_HEADROOM_EV`, `AI_ALO_LOW_DR_EV`, `AI_ALO_STD_DR_EV`, `AI_ALO_HIGH_DR_EV` are removed since the new implementation uses integer P90/P99 thresholds instead of floating-point DR/headroom values.

## Testing Strategy

### Property-Based Tests (via host-side C test harness)

The pure decision logic (zone classification, ALO/HTP resolution, ISO/shutter nudge computation) is extracted into static helper functions that can be compiled and tested on the host (x86) with a property-based testing library (e.g., theft or a custom randomized harness). Each property test generates random valid inputs (P90, P99, raw_iso, raw_shutter, shooting_mode) and verifies the universal properties defined below.

- **Minimum 100 iterations** per property
- **Generator constraints:** P90 ∈ [0, 255], P99 ∈ [0, 255], raw_iso ∈ {72, 80, 88, ..., 128}, raw_shutter ∈ [24, 160] (typical range), shooting_mode ∈ {0, 1, 2, 3}
- **Shrinking:** On failure, reduce inputs to minimal counterexample

### Unit Tests (example-based)

- Zone transitions: verify correct ALO/HTP/ISO at each P90/P99 boundary (79→80, 239→240)
- Setter ordering: confirm `set_htp()` is called before `set_alo()` when transitioning zones
- Feature gate: verify zero side effects when `auto_iso_optimizer == 0`
- Edge case: `ai_light_level()` returns -1 → early exit, no writes

### Integration Tests (on-camera or QEMU)

- Half-shutter fires optimizer exactly once per press cycle
- Verify setter minimization by counting Canon property writes across repeated same-zone presses

## Correctness Properties

*A property is a characteristic or behavior that should hold true across all valid executions of a system — essentially, a formal statement about what the system should do. Properties serve as the bridge between human-readable specifications and machine-verifiable correctness guarantees.*

### Property 1: Zone Classification Correctness

*For any* P90 value in [0, 255] and P99 value in [0, 255], the zone classifier SHALL return Bright if P99 ≥ 240, Dark if P99 < 240 AND P90 < 80, and Normal otherwise — and these three zones are mutually exclusive and exhaustive.

**Validates: Requirements 6.1, 7.1, 8.1**

### Property 2: Dark Zone ALO/HTP Response

*For any* scene classified as Dark_Zone, the optimizer SHALL set HTP to OFF and set ALO to High when P90 ∈ [0, 24], Standard when P90 ∈ [25, 49], or Low when P90 ∈ [50, 79].

**Validates: Requirements 6.2, 6.3, 6.4, 6.5**

### Property 3: Bright Zone ALO/HTP Response

*For any* scene classified as Bright_Zone, the optimizer SHALL set HTP to ON and ALO to Low.

**Validates: Requirements 7.2, 7.3**

### Property 4: Normal Zone Passthrough

*For any* scene classified as Normal_Zone, the optimizer SHALL set ALO to Standard, HTP to ON, and leave both Canon_Raw_ISO and raw_shutter unmodified.

**Validates: Requirements 1.3, 8.2, 8.3, 8.4**

### Property 5: Dark Zone ISO Nudge

*For any* scene classified as Dark_Zone and any current raw_iso value, the optimizer SHALL output a desired raw_iso equal to current raw_iso + 8.

**Validates: Requirements 1.1, 6.6**

### Property 6: Bright Zone ISO Nudge (Above Floor)

*For any* scene classified as Bright_Zone where current raw_iso > 72, the optimizer SHALL output a desired raw_iso equal to current raw_iso − 8 and leave raw_shutter unchanged.

**Validates: Requirements 1.2, 2.2, 7.4**

### Property 7: Shutter Fallback (P Mode at ISO Floor)

*For any* scene classified as Bright_Zone where current raw_iso == 72 and shooting_mode == SHOOTMODE_P, the optimizer SHALL output desired raw_shutter equal to current raw_shutter + 8 and leave raw_iso unchanged.

**Validates: Requirements 2.1, 2.6, 3.4, 7.5**

### Property 8: Non-P Mode Shutter Immutability

*For any* shooting_mode in {SHOOTMODE_AV, SHOOTMODE_TV, SHOOTMODE_M} and any zone/ISO combination, the optimizer SHALL never modify raw_shutter.

**Validates: Requirements 2.3, 2.4, 2.5, 3.1, 3.2, 3.3**

### Property 9: Maximum Exposure Magnitude

*For any* input state, the absolute change in raw_iso SHALL be at most 8, the absolute change in raw_shutter SHALL be at most 8, and at most one of (raw_iso, raw_shutter) SHALL change per invocation.

**Validates: Requirements 1.4**

### Property 10: Aperture Preservation

*For any* input state and any shooting mode, the optimizer SHALL never call `lens_set_rawaperture()` or modify aperture in any way.

**Validates: Requirements 1.5**

### Property 11: Feature Gate

*For any* input state where `auto_iso_optimizer == 0`, the optimizer SHALL not modify ALO, HTP, raw_iso, or raw_shutter (all outputs remain at their pre-call values).

**Validates: Requirements 10.1**

### Property 12: Setter Minimization

*For any* input state, if the computed desired value for a setting (HTP, ALO, ISO, shutter) equals the current value of that setting, the corresponding setter function SHALL not be called.

**Validates: Requirements 12.1, 12.2, 12.3, 12.4**
