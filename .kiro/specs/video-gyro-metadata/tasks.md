# Implementation Plan: Video Gyro Metadata

## Overview

This plan implements per-frame metadata capture during video recording on the Canon EOS 6D. It covers three output paths: Lua-driven CSV/JSON sidecars for H.264 .MOV recordings, C-level gyro bridge exposing IMU data to Lua, and MLV block extensions (GYRO + ETTR) for RAW video. Tasks are ordered to resolve critical build prerequisites first, then build the C infrastructure, then the Lua layer on top.

## Tasks

- [x] 1. Enable CONFIG_VSYNC_EVENTS and verify build
  - [x] 1.1 Uncomment `#define CONFIG_VSYNC_EVENTS` in `source-dev/modules/lua/lua_common.h:32` and verify the build compiles cleanly with `make -C platform/6D.116 -j4`
    - This is the mandatory prerequisite — without it, `event.vsync` never fires and per-frame Lua logging is dead code
    - Verify no compile errors or warnings introduced in lua.c, silent.mo, or mlv_lite.mo
    - _Requirements: 1.4, 2.1, 6.2_

  - [x]* 1.2 Write a minimal Lua test script that registers `event.vsync` and logs a counter to verify the callback fires during LiveView
    - Create `lua_scripts/test_vsync.lua` that increments a global counter on each vsync and prints every 100th frame
    - This validates the build flag works on-device (manual deploy to D:\)
    - _Requirements: 1.4, 9.5_

- [x] 2. Implement Gyro Bridge C module
  - [x] 2.1 Create `source-dev/modules/ettr/gyro_bridge.h` with the public interface
    - Define `gyro_reading_t` struct (int32 x/y/z), `gyro_status_t` enum, and function prototypes per design
    - Include Lua binding prototypes: `luaCB_gyro`, `luaCB_gyro_status`, `luaCB_level`
    - Document the hot-shoe serial protocol (unverified) in header comments
    - _Requirements: 3.1, 3.5, 3.6, 4.5_

  - [x] 2.2 Create `source-dev/modules/ettr/gyro_bridge.c` with stubbed IMU implementation
    - Implement static buffer (`gyro_reading_t`), status variable, init/poll/get/status functions
    - `gyro_bridge_poll()`: stub that returns zeros (hardware unverified) with state machine logic (probing → disconnected → active → timeout transitions)
    - `gyro_bridge_init()`: probe for 100ms, settle to disconnected if no response
    - Implement 2ms timeout logic and 10-consecutive-timeout → disconnected transition
    - All static allocation, no malloc
    - _Requirements: 3.2, 3.4, 3.5, 3.7, 4.1, 4.2, 4.3, 4.4_

  - [x] 2.3 Implement Lua bindings (`luaCB_gyro`, `luaCB_gyro_status`, `luaCB_level`) in `gyro_bridge.c`
    - `mlc.gyro()` → push table {x, y, z} from static buffer (< 50µs)
    - `mlc.gyro_status()` → push string "active"/"timeout"/"disconnected"
    - `mlc.level()` → read Canon PROP_ELECTRONIC_LEVEL, push table {pitch, roll} in degrees
    - _Requirements: 3.1, 3.4, 4.3, 6.4_

  - [x] 2.4 Register Lua bindings in `source-dev/modules/ettr/ettr.c` module init
    - Add `gyro_bridge_init()` call in `ettr_init()`
    - Register `mlc.gyro`, `mlc.gyro_status`, `mlc.level` in the Lua C extension table
    - Add `gyro_bridge_poll()` call in existing `CBR_VSYNC` handler (or register new one if needed)
    - _Requirements: 3.1, 3.3, 4.1_

  - [ ]* 2.5 Write property test for Gyro Bridge state machine (Property 7)
    - **Property 7: Gyro Bridge State Machine**
    - Test all state transitions: active→timeout on single failure, timeout→disconnected on 10 consecutive, any→active on success
    - Verify last-valid-reading retention on timeout
    - **Validates: Requirements 4.2, 4.3**

  - [ ]* 2.6 Write property test for IMU raw scaling range (Property 6)
    - **Property 6: IMU Raw Scaling Range**
    - For any int16 raw value, verify scaled int32 (raw × 10) is within [-2,000,000, +2,000,000] mdps
    - Verify clamping at boundaries
    - **Validates: Requirements 3.6**

  - [ ]* 2.7 Write property test for Gyro Bridge Lua passthrough (Property 5)
    - **Property 5: Gyro Bridge Lua Passthrough**
    - For any gyro_reading_t {x, y, z}, verify mlc.gyro() returns exact same int32 values
    - **Validates: Requirements 3.1, 3.2, 3.7**

- [x] 3. Checkpoint - Verify gyro bridge builds and compiles
  - Ensure all tests pass, ask the user if questions arise.

- [x] 4. Implement MLV Block Extensions
  - [x] 4.1 Create `source-dev/modules/ettr/mlv_metadata.h` with block struct definitions
    - Define `mlv_gyro_hdr_t` and `mlv_ettr_hdr_t` packed structs per design
    - Include `mlv.h` and `mlv_rec_interface.h` from raw_video module
    - Declare `mlv_metadata_init()` function
    - _Requirements: 7.2, 7.3, 7.4, 8.2, 8.3, 8.4_

  - [x] 4.2 Create `source-dev/modules/ettr/mlv_metadata.c` with block writer implementation
    - Implement static block pool (pre-allocated GYRO + ETTR block instances)
    - Register `MLV_REC_EVENT_VIDF` callback via `mlv_rec_register_cbr()`
    - On each VIDF event: check gyro status → build GYRO block if active; check ETTR enabled → build ETTR block
    - Enqueue blocks via `mlv_rec_queue_block()` (non-blocking)
    - Maintain frame counter (uint32, 0-indexed per recording)
    - _Requirements: 7.1, 7.5, 7.6, 8.1, 8.5, 8.6, 9.4, 11.4_

  - [x] 4.3 Wire `mlv_metadata_init()` into `ettr_init()` in `ettr.c`
    - Call `mlv_metadata_init()` at module load
    - Ensure it gracefully handles case where mlv_rec module is not loaded
    - _Requirements: 7.1, 8.1_

  - [ ]* 4.4 Write property test for MLV GYRO block binary round-trip (Property 8)
    - **Property 8: MLV GYRO Block Binary Round-Trip**
    - For any frame_number (uint32) and gyro_reading_t, verify constructing mlv_gyro_hdr_t and reading back fields at byte offsets yields original values
    - **Validates: Requirements 7.3**

  - [ ]* 4.5 Write property test for MLV ETTR block conversion round-trip (Property 9)
    - **Property 9: MLV ETTR Block Conversion Round-Trip**
    - For any valid ettr_metadata_t, verify integer conversion (sceneDR×100, clip×10000) round-trips within ±1
    - **Validates: Requirements 8.3**

  - [ ]* 4.6 Write property test for MLV block size invariant (Property 10)
    - **Property 10: MLV Custom Block Size Invariant**
    - For any constructed GYRO or ETTR block, verify blockSize == sizeof(mlv_hdr_t) + 16 and first 4 bytes == ASCII block type
    - **Validates: Requirements 7.4, 8.4, 11.3**

  - [ ]* 4.7 Write property test for conditional block emission (Property 11)
    - **Property 11: Conditional Block Emission**
    - Verify GYRO block emitted iff gyro_bridge_status() == ACTIVE; ETTR block emitted iff auto_ettr != 0
    - **Validates: Requirements 7.5, 8.6, 12.2**

- [x] 5. Checkpoint - Verify MLV extensions compile and link
  - Ensure all tests pass, ask the user if questions arise.

- [x] 6. Rewrite MOV Metadata Lua Script
  - [x] 6.1 Create the core `lua_scripts/mov_metadata.lua` framework with record start/stop detection
    - Use `event.shoot_task` + `movie.recording` polling for edge detection (Option A from design)
    - On rising edge: derive file paths from `dryos.shooting_card.file_number`, create `ML/DATA/SHOTS/` dir, open all 3 output files
    - On falling edge: flush Gyroflow JSON array to file, close all handles, show 3s notification with filename + frame count
    - Implement menu entry: "MOV Metadata" under Expo, choices OFF/ON, persisted via ML config
    - _Requirements: 1.1, 1.2, 1.5, 1.6, 10.1, 10.2, 10.3, 10.4_

  - [x] 6.2 Implement per-frame CSV logging in `event.vsync` handler
    - Write schema version comment `# schema_version=1.0` and `# gyro_source=...` and `# fps=...` at file open
    - Write CSV header row: `timestamp_s,frame,iso,shutter_raw,wb_r,wb_g,wb_b,lens_id,focus_dist,light_level,gyro_x,gyro_y,gyro_z`
    - Per-frame: compute timestamp as `frame_index / fps`, read camera state, call `mlc.gyro()`, convert mdps to rad/s, format row
    - Use pre-allocated 256-byte string buffer pattern (string.format with reused pattern)
    - Implement 1ms write timeout logic (skip frame on slow write)
    - _Requirements: 1.3, 1.4, 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 9.1, 9.2, 9.3, 9.6, 11.1, 11.2, 12.4_

  - [x] 6.3 Implement Gyroflow CSV and JSON writers
    - Gyroflow CSV: write `time,gyroX,gyroY,gyroZ,pitch,roll` header, then per-frame rows with rad/s gyro + degrees pitch/roll from `mlc.level()`
    - Gyroflow JSON: accumulate frame entries in Lua table, flush complete JSON on record stop
    - JSON includes: `fps`, `units: "rad/s"`, `extra: "Canon 6D electronic level metadata"`
    - Handle disconnected gyro: write zeros for gyro, actual values for pitch/roll
    - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5, 6.6, 6.7, 6.8, 6.9, 6.10, 6.11_

  - [ ]* 6.4 Write property test for output file path derivation (Property 1)
    - **Property 1: Output File Path Derivation**
    - For any file_number 0–9999, verify three paths produced with correct zero-padded basename
    - **Validates: Requirements 1.1, 6.1, 6.5, 6.9**

  - [ ]* 6.5 Write property test for frame-aligned timestamp spacing (Property 2)
    - **Property 2: Frame-Aligned Timestamp Spacing**
    - For any frame_index and fps ∈ {24,25,30,50,60}, verify timestamp == N/fps and consecutive diff == 1/fps within epsilon
    - **Validates: Requirements 2.1, 6.2**

  - [ ]* 6.6 Write property test for mdps to rad/s conversion round-trip (Property 3)
    - **Property 3: Milli-Degrees-Per-Second to Radians-Per-Second Conversion Round-Trip**
    - For any int32 mdps in [-2M, +2M], verify round-trip within ±1 of original
    - **Validates: Requirements 6.3**

  - [ ]* 6.7 Write property test for Gyroflow CSV row format invariant (Property 4)
    - **Property 4: Gyroflow CSV Row Format Invariant**
    - For any gyro reading, verify formatted row has exactly 5 commas, no quotes, ends with LF, 6 decimal places
    - **Validates: Requirements 6.4**

  - [ ]* 6.8 Write property test for graceful degradation (Property 12)
    - **Property 12: Graceful Degradation — Gyro Zeros with Intact Non-Gyro Data**
    - When gyro disconnected, verify non-gyro columns populated correctly and gyro columns == 0.000000
    - **Validates: Requirements 12.1, 6.8**

- [x] 7. Checkpoint - Full build verification
  - Ensure all tests pass, ask the user if questions arise.

- [x] 8. Integration and wiring
  - [x] 8.1 Update `source-dev/modules/ettr/Makefile` (or module build rules) to compile `gyro_bridge.c` and `mlv_metadata.c` into `ettr.mo`
    - Add new .c files to the module's source list
    - Verify the final `ettr.mo` links without unresolved symbols
    - _Requirements: 3.1, 7.1, 8.1_

  - [x] 8.2 Verify end-to-end build produces working `ettr.mo` with all new symbols
    - Run full `make -C platform/6D.116 -j4` and confirm autoexec.bin + ettr.mo output
    - Check that `gyro_bridge_init`, `mlv_metadata_init`, `luaCB_gyro` symbols exist in .mo
    - _Requirements: all_

  - [x] 8.3 Deploy `mov_metadata.lua` to `ML/scripts/` in the on-card structure
    - Ensure the script is copied to the SD card deploy path alongside other Lua scripts
    - Verify menu entry appears under Expo when lua.mo loads the script
    - _Requirements: 10.1, 10.3_

- [x] 9. Final checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

## Notes

- Tasks marked with `*` are optional and can be skipped for faster MVP
- Each task references specific requirements for traceability
- Checkpoints ensure incremental validation
- Property tests validate universal correctness properties from the design document
- Unit tests validate specific examples and edge cases
- The gyro bridge stubs to zeros until hardware is verified with oscilloscope — this is NOT a blocker
- The hot-shoe serial implementation is a placeholder; real IMU data flows once hardware is confirmed
- Build command: `wsl bash -c "cd /mnt/c/Users/Public/Kiro/ML_6D/source-dev && make -C platform/6D.116 -j4"`

## Task Dependency Graph

```json
{
  "waves": [
    { "id": 0, "tasks": ["1.1"] },
    { "id": 1, "tasks": ["1.2", "2.1"] },
    { "id": 2, "tasks": ["2.2", "4.1"] },
    { "id": 3, "tasks": ["2.3", "4.2"] },
    { "id": 4, "tasks": ["2.4", "4.3"] },
    { "id": 5, "tasks": ["2.5", "2.6", "2.7", "4.4", "4.5", "4.6", "4.7"] },
    { "id": 6, "tasks": ["6.1", "8.1"] },
    { "id": 7, "tasks": ["6.2"] },
    { "id": 8, "tasks": ["6.3"] },
    { "id": 9, "tasks": ["6.4", "6.5", "6.6", "6.7", "6.8"] },
    { "id": 10, "tasks": ["8.2"] },
    { "id": 11, "tasks": ["8.3"] }
  ]
}
```
