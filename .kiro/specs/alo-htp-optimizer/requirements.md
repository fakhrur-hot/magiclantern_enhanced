# Requirements Document

## Introduction

The ALO/HTP Scene Optimizer replaces the existing `ai_apply_alo_htp()` function in the Canon EOS 6D Magic Lantern AI module. The implementation uses integer-based raw histogram percentiles (P90, P99 on the 0–255 green channel) to classify scenes into three brightness zones and drives Canon's Auto Lighting Optimizer (ALO), Highlight Tone Priority (HTP), and a minimal single-step (±1/3 EV) ISO nudge accordingly. The optimizer fires once per half-shutter press and limits exposure intervention to exactly one Canon ISO step (8 raw units = 1/3 EV), with a shutter-speed fallback when ISO cannot be lowered further in the Bright Zone.

## Glossary

- **Scene_Optimizer**: The replacement `ai_apply_alo_htp()` function that classifies scene brightness and sets ALO, HTP, and applies a minimal ISO/shutter nudge
- **P90**: The 90th-percentile value (0–255) of the raw histogram green channel, representing overall scene brightness
- **P99**: The 99th-percentile value (0–255) of the raw histogram green channel, representing highlight peak intensity; populated in the global `ai_raw_p99`
- **ALO**: Canon Auto Lighting Optimizer — a JPEG/CR2 tone curve adjustment that lifts shadows and midtones; four levels: Off, Low, Standard, High
- **HTP**: Canon Highlight Tone Priority — a tone curve remap that protects highlights at the cost of raising the ISO floor to 200
- **Half_Shutter_Press**: The camera event where the shutter button is pressed halfway, triggering autofocus and metering
- **AI_Light_Level**: The AI model inference function `ai_light_level()` that returns P90 green (0–255) and populates `ai_raw_p99`
- **Dark_Zone**: Scene classification where P90 falls below the darkness threshold (P90 < 80), indicating insufficient ambient light
- **Bright_Zone**: Scene classification where P99 reaches or exceeds 240, indicating highlight overexposure risk
- **Normal_Zone**: Scene classification where neither Dark_Zone nor Bright_Zone conditions are met
- **Auto_ISO_Optimizer**: The existing `auto_iso_optimizer` toggle that gates the Scene_Optimizer feature
- **Canon_Raw_ISO**: The raw ISO value as represented on Canon's internal scale; each step of 8 raw units equals 1/3 EV; raw 72 = ISO 100 (floor), raw 80 = ISO 125
- **ISO_Nudge**: A single-step ISO adjustment of exactly 8 raw units (1/3 EV) applied to the current Canon_Raw_ISO
- **Shutter_Fallback**: An increase of 8 raw shutter units (1/3 EV faster shutter) applied when ISO cannot be lowered below the floor in Bright_Zone
- **ISO_Floor**: The minimum ISO value below which the camera cannot go; raw 72 = ISO 100 for non-HTP operation
- **Shooting_Mode**: The camera's current exposure mode: M (Manual), Av (Aperture Priority), Tv (Shutter Priority), or P (Program)

## Requirements

### Requirement 1: Minimal Exposure Nudge

**User Story:** As a photographer, I want the Scene_Optimizer to apply a protective single-step ISO nudge (±1/3 EV) in clearly dark or bright scenes, so that shadows get slightly more signal in dark conditions and highlights are slightly better protected in bright conditions, without aggressive ETTR-style exposure overrides.

#### Acceptance Criteria

1. WHEN the scene is classified as Dark_Zone, THE Scene_Optimizer SHALL increase Canon_Raw_ISO by exactly 8 raw units (one step = +1/3 EV)
2. WHEN the scene is classified as Bright_Zone, THE Scene_Optimizer SHALL decrease Canon_Raw_ISO by exactly 8 raw units (one step = -1/3 EV)
3. WHEN the scene is classified as Normal_Zone, THE Scene_Optimizer SHALL preserve the Canon_Raw_ISO without modification
4. THE Scene_Optimizer SHALL limit exposure modification to a maximum magnitude of 1/3 EV (8 raw ISO units) per Half_Shutter_Press
5. THE Scene_Optimizer SHALL preserve the Canon aperture value without modification in all Shooting_Modes

### Requirement 2: Shutter Fallback in Bright Zone

**User Story:** As a photographer shooting in bright conditions where ISO is already at the floor, I want the optimizer to use a slightly faster shutter speed instead, so that highlights are still protected when ISO cannot be lowered further.

#### Acceptance Criteria

1. WHEN the scene is classified as Bright_Zone AND Canon_Raw_ISO equals the ISO_Floor (raw 72), THE Scene_Optimizer SHALL increase raw_shutter by 8 units (1/3 EV faster) instead of decreasing ISO
2. WHEN the scene is classified as Bright_Zone AND Canon_Raw_ISO is above the ISO_Floor, THE Scene_Optimizer SHALL decrease Canon_Raw_ISO by 8 raw units and leave shutter unchanged
3. WHILE the Shooting_Mode is Tv (Shutter Priority), THE Scene_Optimizer SHALL not modify raw_shutter regardless of ISO_Floor status
4. WHILE the Shooting_Mode is Av (Aperture Priority), THE Scene_Optimizer SHALL not modify raw_shutter regardless of ISO_Floor status
5. WHILE the Shooting_Mode is M (Manual), THE Scene_Optimizer SHALL not modify raw_shutter regardless of ISO_Floor status
6. WHEN the Shooting_Mode is P (Program) AND Canon_Raw_ISO equals the ISO_Floor AND the scene is Bright_Zone, THE Scene_Optimizer SHALL apply the Shutter_Fallback

### Requirement 3: Mode-Specific Constraints

**User Story:** As a photographer using different exposure modes, I want the optimizer to respect each mode's user-controlled parameters, so that my aperture in Av, my shutter in Tv, and my manual settings in M are never overridden.

#### Acceptance Criteria

1. WHILE the Shooting_Mode is Av, THE Scene_Optimizer SHALL adjust only Canon_Raw_ISO (never aperture or shutter)
2. WHILE the Shooting_Mode is Tv, THE Scene_Optimizer SHALL adjust only Canon_Raw_ISO (never shutter or aperture)
3. WHILE the Shooting_Mode is M, THE Scene_Optimizer SHALL adjust only Canon_Raw_ISO (never shutter or aperture)
4. WHILE the Shooting_Mode is P, THE Scene_Optimizer SHALL adjust Canon_Raw_ISO when possible, and apply Shutter_Fallback only when ISO is at the ISO_Floor in Bright_Zone

### Requirement 4: Half-Shutter Trigger

**User Story:** As a photographer, I want the Scene_Optimizer to analyze and apply settings on half-shutter press, so that the optimization happens once per shot attempt without delaying my capture.

#### Acceptance Criteria

1. WHEN a Half_Shutter_Press event is detected, THE Scene_Optimizer SHALL perform exactly one scene brightness analysis and apply settings once
2. THE Scene_Optimizer SHALL use the existing `ai_logged_press` pattern to ensure execution occurs exactly once per Half_Shutter_Press
3. THE Scene_Optimizer SHALL perform scene analysis only on Half_Shutter_Press events, not on any other camera event

### Requirement 5: Scene Brightness Analysis

**User Story:** As a photographer, I want the optimizer to read the raw histogram to determine scene brightness, so that ALO/HTP/ISO decisions are based on actual sensor data.

#### Acceptance Criteria

1. WHEN a scene analysis is triggered, THE Scene_Optimizer SHALL obtain the P90 green channel value (0–255) from the raw histogram as the primary brightness indicator
2. WHEN a scene analysis is triggered, THE Scene_Optimizer SHALL obtain the P99 green channel value (0–255) from `ai_raw_p99` as the highlight clipping indicator
3. WHEN the raw histogram is unavailable or ambiguous, THE Scene_Optimizer SHALL use the AI_Light_Level inference result as a tiebreaker for zone classification

### Requirement 6: Dark Zone Classification and Response

**User Story:** As a photographer shooting in dim conditions, I want ALO to automatically increase shadow lift proportional to scene darkness and receive a minimal ISO boost, so that my low-light images retain shadow detail without enabling HTP's ISO floor penalty.

#### Acceptance Criteria

1. WHEN P90 falls below 80, THE Scene_Optimizer SHALL classify the scene as Dark_Zone
2. WHEN the scene is classified as Dark_Zone with slight darkness severity (P90 in [50, 79]), THE Scene_Optimizer SHALL set ALO to Low
3. WHEN the scene is classified as Dark_Zone with moderate darkness severity (P90 in [25, 49]), THE Scene_Optimizer SHALL set ALO to Standard
4. WHEN the scene is classified as Dark_Zone with very dark severity (P90 in [0, 24]), THE Scene_Optimizer SHALL set ALO to High
5. WHILE the scene is classified as Dark_Zone, THE Scene_Optimizer SHALL set HTP to OFF
6. WHEN the scene is classified as Dark_Zone, THE Scene_Optimizer SHALL increase Canon_Raw_ISO by 8 raw units (+1/3 EV) to provide additional shadow signal

### Requirement 7: Bright Zone Classification and Response

**User Story:** As a photographer shooting highlight-critical scenes, I want HTP to engage automatically and ISO to drop one step when highlights approach clipping, so that blown highlights are prevented by both the Canon tone curve remap and reduced sensor exposure.

#### Acceptance Criteria

1. WHEN P99 is greater than or equal to 240, THE Scene_Optimizer SHALL classify the scene as Bright_Zone
2. WHEN the scene is classified as Bright_Zone, THE Scene_Optimizer SHALL set HTP to ON
3. WHEN the scene is classified as Bright_Zone, THE Scene_Optimizer SHALL set ALO to Low only
4. WHEN the scene is classified as Bright_Zone AND Canon_Raw_ISO is above the ISO_Floor, THE Scene_Optimizer SHALL decrease Canon_Raw_ISO by 8 raw units (-1/3 EV)
5. WHEN the scene is classified as Bright_Zone AND Canon_Raw_ISO equals the ISO_Floor, THE Scene_Optimizer SHALL apply Shutter_Fallback per Requirement 2

### Requirement 8: Normal Zone Classification and Response

**User Story:** As a photographer shooting in typical conditions, I want a balanced ALO and HTP combination applied automatically with no exposure change, so that both shadow detail and highlight protection are maintained without altering Canon's metering.

#### Acceptance Criteria

1. WHEN P90 is at or above 80 AND P99 is below 240, THE Scene_Optimizer SHALL classify the scene as Normal_Zone
2. WHEN the scene is classified as Normal_Zone, THE Scene_Optimizer SHALL set ALO to Standard
3. WHEN the scene is classified as Normal_Zone, THE Scene_Optimizer SHALL set HTP to ON
4. WHEN the scene is classified as Normal_Zone, THE Scene_Optimizer SHALL preserve Canon_Raw_ISO and raw_shutter without modification

### Requirement 9: Hardware Mutual Exclusion Compliance

**User Story:** As a firmware developer, I want the optimizer to respect Canon's hardware constraint that ALO is suppressed when HTP is active, so that no conflicting register states are written.

#### Acceptance Criteria

1. THE Scene_Optimizer SHALL apply ALO level changes only when HTP is OFF or being set to OFF in the same decision cycle
2. WHEN HTP is set to ON, THE Scene_Optimizer SHALL accept that Canon firmware suppresses ALO regardless of the ALO register value
3. THE Scene_Optimizer SHALL call `set_htp()` before `set_alo()` when transitioning between zones that change both settings

### Requirement 10: Feature Gating

**User Story:** As a photographer, I want the Scene_Optimizer to operate only when the AI ISO Optimizer is enabled, so that I retain full manual control when the feature is toggled off.

#### Acceptance Criteria

1. WHILE the Auto_ISO_Optimizer toggle is disabled, THE Scene_Optimizer SHALL not execute scene analysis or modify ALO/HTP/ISO/shutter settings
2. WHEN the Auto_ISO_Optimizer toggle is enabled, THE Scene_Optimizer SHALL become active on the next Half_Shutter_Press event

### Requirement 11: Integer-Only Computation

**User Story:** As a firmware developer, I want the optimizer to use integer arithmetic exclusively, so that shared ARM Cortex-R4 code paths avoid floating-point overhead and remain compatible with the Lua coexistence constraint.

#### Acceptance Criteria

1. THE Scene_Optimizer SHALL perform all threshold comparisons, zone classification, and ISO/shutter arithmetic using integer operations on raw Canon values
2. THE Scene_Optimizer SHALL not use floating-point types or operations in any code path
3. THE Scene_Optimizer SHALL not perform heap allocation during scene analysis or setting application

### Requirement 12: Setter Minimization

**User Story:** As a firmware developer, I want the optimizer to write registers only when the desired value differs from the current state, so that unnecessary Canon property writes are avoided.

#### Acceptance Criteria

1. WHEN the desired HTP state matches the current HTP state returned by `get_htp()`, THE Scene_Optimizer SHALL skip the `set_htp()` call
2. WHEN the desired ALO level matches the current ALO level returned by `get_alo()`, THE Scene_Optimizer SHALL skip the `set_alo()` call
3. WHEN the desired Canon_Raw_ISO matches the current `lens_info.raw_iso`, THE Scene_Optimizer SHALL skip the `lens_set_rawiso()` call
4. WHEN the desired raw_shutter matches the current `lens_info.raw_shutter`, THE Scene_Optimizer SHALL skip the `lens_set_rawshutter()` call
