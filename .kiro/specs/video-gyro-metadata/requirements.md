# Requirements Document

## Introduction

This feature adds video recording metadata support to the ML_6D firmware. It comprises three subsystems: (1) a Lua script that logs per-frame metadata to a CSV sidecar file alongside .MOV recordings, (2) a C module that exposes gyro/IMU sensor readings to Lua, and (3) MLV block extensions that embed gyro and ETTR metadata directly into RAW video files. All outputs are designed for consumption by RaZStudio and gyroflow-compatible stabilization software.

## Glossary

- **MOV_Metadata_Logger**: The Lua script responsible for writing per-frame CSV sidecar files alongside Canon H.264 .MOV recordings
- **Gyro_Bridge**: The C module that exposes gyro/IMU sensor readings to Lua via the `mlc.gyro()` function call
- **MLV_Recorder**: The existing `mlv_lite.mo` module that handles RAW video (.MLV) recording, extended with custom metadata blocks
- **ETTR_Module**: The existing `ettr.mo` module that computes scene DR, highlight headroom, and per-channel clip fractions via `ettr_compute_extended_metadata()`
- **CSV_Sidecar**: A comma-separated values file written alongside each .MOV recording containing per-frame metadata
- **GYRO_Block**: A custom MLV block (4-char ID "GYRO") containing per-frame gyroscope readings and timestamp
- **ETTR_Block**: A custom MLV block (4-char ID "ETTR") containing per-frame scene analysis metadata from the ETTR module
- **External_IMU**: An optional external inertial measurement unit (e.g., MPU6050) connected via hot-shoe serial/GPIO
- **Gyroflow_Format**: The CSV format specification used by the gyroflow open-source stabilization software
- **mlv_hdr_t**: The standard MLV block header structure containing blockType (4 bytes), blockSize (uint32), and timestamp (uint64)
- **Frame_Cadence**: The rate at which metadata is sampled, matching the video frame rate (24–60 Hz)

## Requirements

### Requirement 1: MOV Recording CSV Sidecar Creation

**User Story:** As a filmmaker, I want per-frame metadata logged to a CSV file alongside my .MOV recordings, so that I can use the data for post-production stabilization and color grading in RaZStudio or DaVinci Resolve.

#### Acceptance Criteria

1. WHILE the MOV Metadata menu is set to "ON", WHEN a .MOV recording starts, THE MOV_Metadata_Logger SHALL create (or overwrite if already existing) a CSV sidecar file at `ML/DATA/SHOTS/{MOV_BASENAME}.csv`, where MOV_BASENAME is derived from the camera's current file number in the format `MVI_NNNN`
2. WHEN a .MOV recording stops, THE MOV_Metadata_Logger SHALL flush and close the CSV sidecar file and display a non-blocking notification for 3 seconds showing the filename and total frame count written
3. THE MOV_Metadata_Logger SHALL write one CSV header row as the first data line containing the columns: `timestamp_s,frame,iso,shutter_raw,wb_r,wb_g,wb_b,lens_id,focus_dist,light_level,gyro_x,gyro_y,gyro_z`
4. WHILE recording is active, THE MOV_Metadata_Logger SHALL append one data row per video frame at the frame cadence (24–60 Hz depending on video mode), with each row flushed or written such that completed rows persist even if recording is interrupted by power loss
5. IF the CSV sidecar file cannot be opened for writing (directory missing, card full, or write-protected), THEN THE MOV_Metadata_Logger SHALL display a non-blocking notification indicating the failure reason and continue recording without metadata logging for the duration of that clip
6. IF the `ML/DATA/SHOTS/` directory does not exist at recording start, THEN THE MOV_Metadata_Logger SHALL create it before attempting to open the CSV file

### Requirement 2: Per-Frame Metadata Content

**User Story:** As a colorist, I want accurate per-frame exposure and lens metadata in the CSV sidecar, so that I can reconstruct the camera state for each frame during color grading.

#### Acceptance Criteria

1. THE MOV_Metadata_Logger SHALL record the timestamp in milliseconds elapsed since recording start for each frame row
2. THE MOV_Metadata_Logger SHALL record the current ISO raw value from `camera.iso.raw` for each frame row
3. THE MOV_Metadata_Logger SHALL record the current shutter speed raw value from `camera.shutter.raw` for each frame row
4. THE MOV_Metadata_Logger SHALL record white balance gains (R, G, B) from `lens.wb_r`, `lens.wb_g`, `lens.wb_b` for each frame row
5. THE MOV_Metadata_Logger SHALL record the lens ID from `lens.id` for each frame row
6. THE MOV_Metadata_Logger SHALL record the focus distance from `lens.focus_distance` for each frame row

### Requirement 3: Gyro Bridge C Module

**User Story:** As a firmware developer, I want a C bridge that exposes gyro/IMU sensor readings to Lua, so that the MOV metadata logger and other Lua scripts can access motion data.

#### Acceptance Criteria

1. THE Gyro_Bridge SHALL expose a Lua-callable function `mlc.gyro()` that returns three signed 32-bit integer values representing angular velocity on X, Y, and Z axes in units of milli-degrees per second (mdps)
2. WHEN no External_IMU is connected or detected, THE Gyro_Bridge SHALL return zero for all three axes
3. WHEN an External_IMU is connected and the Gyro_Bridge status is "active", THE Gyro_Bridge SHALL return the most recent gyro reading, which SHALL be no older than one frame period (16.7 ms at 60 fps to 41.7 ms at 24 fps)
4. THE Gyro_Bridge SHALL complete the `mlc.gyro()` call within 50 microseconds to avoid blocking the Lua frame callback
5. THE Gyro_Bridge SHALL use no dynamic heap allocation (all buffers statically allocated)
6. THE Gyro_Bridge SHALL use integer-only arithmetic on the code path shared with Lua, with gyro values represented as signed 32-bit integers in milli-degrees per second (valid range: -2,000,000 to +2,000,000 mdps, corresponding to ±2000 degrees per second full-scale)
7. IF `mlc.gyro()` is called before the Gyro_Bridge has completed initialization, THEN THE Gyro_Bridge SHALL return zero for all three axes without error

### Requirement 4: External IMU Hardware Interface

**User Story:** As a hardware integrator, I want the gyro bridge to support external IMU sensors via the hot-shoe serial interface, so that Canon 6D users can add gyro capability to their camera.

#### Acceptance Criteria

1. THE Gyro_Bridge SHALL poll the external IMU serial interface once per video frame to read the latest gyro sample
2. IF the serial interface returns no data within a 2-millisecond timeout, THEN THE Gyro_Bridge SHALL use the previous valid reading or zero if no valid reading has been received
3. THE Gyro_Bridge SHALL provide a status function `mlc.gyro_status()` returning one of: "active", "timeout", or "disconnected"
4. WHEN the Gyro_Bridge initializes, THE Gyro_Bridge SHALL probe the serial interface and set the status to "disconnected" if no IMU responds within 100 milliseconds
5. THE Gyro_Bridge SHALL document the required external hardware connection (pin mapping, baud rate, protocol) in a header comment

### Requirement 5: Canon IS Lens Vibration Data (Optional)

**User Story:** As a researcher, I want the gyro bridge to attempt reading vibration compensation data from Canon IS lenses via the lens communication protocol, so that users with IS lenses may get partial motion data without external hardware.

#### Acceptance Criteria

1. WHERE the Canon IS lens communication protocol exposes vibration data, THE Gyro_Bridge SHALL read and convert lens vibration readings to the same integer format as External_IMU readings
2. WHERE no IS lens vibration data is available (non-IS lens or protocol not accessible), THE Gyro_Bridge SHALL fall back to External_IMU or zero readings without error
3. THE Gyro_Bridge SHALL document which lens communication registers are accessed and note that this interface is undocumented and experimental

### Requirement 6: Gyroflow-Native Log Format with Electronic Level

**User Story:** As a stabilization user, I want the gyro log to be directly importable into Gyroflow (and its plugins for Premiere, Resolve, Final Cut), with optional Canon 6D electronic level (pitch/roll) orientation tags for horizon correction.

#### Acceptance Criteria

1. THE MOV_Metadata_Logger SHALL write a Gyroflow-native CSV log at `ML/DATA/SHOTS/{MOV_BASENAME}.gyro.csv` containing columns: `time,gyroX,gyroY,gyroZ,pitch,roll`
2. THE gyro CSV SHALL use `time` as seconds since recording start computed as `frame_index / fps` (frame-aligned, e.g., `0.041667` for frame 1 at 24 fps), guaranteeing exact 1/fps spacing regardless of Lua callback timing jitter
3. THE gyro CSV SHALL write `gyroX`, `gyroY`, `gyroZ` values in radians per second (converted from the Gyro_Bridge milli-degrees-per-second output: `rad/s = mdps * π / 180000`)
4. THE gyro CSV SHALL write `pitch` and `roll` values in degrees from the Canon EOS electronic level sensor (accelerometer-based, available on 6D, 5D3, 5D4, 7D2, 70D, 80D, 77D and other legacy EOS bodies with virtual horizon; updated at sensor cadence ~5 Hz, hold-last-value interpolated to frame cadence)
5. THE gyro CSV SHALL use comma as the field separator and newline (LF) as the row terminator, with no quoting or padding
6. THE MOV_Metadata_Logger SHALL ALSO write a Gyroflow JSON companion file at `ML/DATA/SHOTS/{MOV_BASENAME}.gyro.json` containing:
   ```json
   {
     "gyro": [
       {"t":0.000000,"x":0.0123,"y":-0.0045,"z":0.0012,"pitch":1.5,"roll":-0.3},
       ...
     ],
     "fps": 24.0,
     "units": "rad/s",
     "extra": "Canon 6D electronic level metadata"
   }
   ```
7. THE JSON `fps` field SHALL reflect the actual video frame rate at recording start (24, 25, 30, 50, or 60)
8. THE JSON `units` field SHALL always be `"rad/s"` (applies to gyroX/Y/Z; pitch/roll are always degrees)
9. WHEN gyro data is unavailable (Gyro_Bridge status is "disconnected"), THE MOV_Metadata_Logger SHALL still write both files with gyro values set to `0.000000` and pitch/roll set to the actual electronic level readings (orientation metadata is available on all EOS bodies with virtual horizon — 6D, 5D3, 5D4, 7D2, 70D, 80D, etc. — without external hardware)
10. THE gyro log file names SHALL match the MOV clip name so that Gyroflow auto-discovers them when the .MOV is loaded (e.g., `MVI_0042.MOV` → `MVI_0042.gyro.csv`)
11. WHEN the electronic level sensor returns invalid data, THE MOV_Metadata_Logger SHALL write `0.000` for pitch and roll

### Requirement 7: MLV GYRO Block Extension

**User Story:** As a RAW video user, I want per-frame gyroscope data embedded in my MLV files, so that stabilization software can read motion data directly from the video container without a separate sidecar file.

#### Acceptance Criteria

1. WHILE MLV recording is active and the Gyro_Bridge status is "active", THE MLV_Recorder SHALL write one GYRO_Block per video frame
2. THE GYRO_Block SHALL use the block type identifier "GYRO" (4 ASCII characters) and begin with a standard `mlv_hdr_t` header (blockType, blockSize, timestamp)
3. THE GYRO_Block SHALL contain the following fields after the header: frameNumber (uint32, 0-indexed starting from the first recorded frame, monotonically increasing), gyroX (int32), gyroY (int32), gyroZ (int32) — all gyro values in milli-degrees per second within the range −2,147,483,648 to 2,147,483,647
4. THE GYRO_Block SHALL have a fixed blockSize equal to `sizeof(mlv_hdr_t) + 16` bytes (header + 4 × uint32/int32 fields)
5. IF the Gyro_Bridge status is "disconnected" or "timeout" for a given frame, THEN THE MLV_Recorder SHALL omit the GYRO_Block entry for that frame rather than writing zeros
6. WHEN the Gyro_Bridge status transitions from "timeout" or "disconnected" back to "active" during an MLV recording, THE MLV_Recorder SHALL resume writing GYRO_Block entries starting with the next video frame, using the frameNumber that corresponds to that frame's position in the recording sequence

### Requirement 8: MLV ETTR Block Extension

**User Story:** As a colorist working with MLV RAW video, I want per-frame scene analysis metadata (dynamic range, highlight headroom, clip fractions) embedded in the MLV file, so that I can make informed grading decisions per frame.

#### Acceptance Criteria

1. WHILE MLV recording is active and ETTR is enabled, THE MLV_Recorder SHALL write one ETTR_Block per video frame
2. THE ETTR_Block SHALL use the block type identifier "ETTR" (4 ASCII characters) and begin with a standard `mlv_hdr_t` header (blockType, blockSize, timestamp)
3. THE ETTR_Block SHALL contain the following fields after the header: frameNumber (uint32), sceneDR (int16, in 1/100 EV units), highlightHeadroom (int16, in 1/100 EV units), clipR (uint16, 0–10000 representing 0.0–1.0), clipG (uint16, 0–10000), clipB (uint16, 0–10000), evBias (int16, in 1/8 EV units)
4. THE ETTR_Block SHALL have a fixed blockSize equal to `sizeof(mlv_hdr_t) + 16` bytes (header + uint32 frameNumber + 6 × int16/uint16 fields)
5. THE ETTR_Block SHALL source its values from the existing `ettr_compute_extended_metadata()` function output, converted to the integer representation specified above
6. WHEN ETTR is disabled, THE MLV_Recorder SHALL omit ETTR_Block entries entirely

### Requirement 9: Recording Performance Safety

**User Story:** As a filmmaker, I want metadata logging to never cause dropped frames during video recording, so that my footage is always complete and usable.

#### Acceptance Criteria

1. THE MOV_Metadata_Logger SHALL complete each CSV row write within 1 millisecond; IF the write has not completed within 1 millisecond, THEN THE MOV_Metadata_Logger SHALL treat it as blocking and abandon that row
2. IF a CSV write operation exceeds the 1-millisecond threshold, THEN THE MOV_Metadata_Logger SHALL skip that frame's metadata row and continue recording without delaying the video pipeline
3. IF the MOV_Metadata_Logger skips more than 10 consecutive metadata rows due to write delays, THEN THE MOV_Metadata_Logger SHALL display a single non-blocking notification indicating metadata logging degradation
4. THE MLV_Recorder SHALL write GYRO_Block and ETTR_Block data using the existing MLV block queuing mechanism (`mlv_rec_queue_block`) which handles write scheduling without frame drops
5. THE Gyro_Bridge SHALL complete all per-frame operations (IMU read + Lua return) within 5% of one frame period (i.e., no more than 2.08 ms at 24 fps, scaling down to 0.83 ms at 60 fps)
6. THE MOV_Metadata_Logger SHALL pre-allocate a string buffer of at least 256 bytes for CSV row formatting rather than performing per-frame memory allocation

### Requirement 10: Menu Integration

**User Story:** As a camera operator, I want a menu toggle to enable or disable MOV metadata logging, so that I can control when sidecar files are generated.

#### Acceptance Criteria

1. THE MOV_Metadata_Logger SHALL provide a menu entry under the "Expo" menu named "MOV Metadata" with choices "OFF" and "ON"
2. WHEN the menu is set to "OFF", THE MOV_Metadata_Logger SHALL not create CSV sidecar files or perform per-frame logging during .MOV recording
3. WHEN the menu is set to "ON" and recording starts, THE MOV_Metadata_Logger SHALL begin logging immediately from the first frame
4. THE MOV_Metadata_Logger SHALL persist the menu setting across camera power cycles using the ML config system

### Requirement 11: RaZStudio Video Metadata Consumption

**User Story:** As a RaZStudio user, I want to import video metadata from both CSV sidecars (.MOV) and MLV blocks (.MLV), so that I can apply AI-assisted corrections to video footage.

#### Acceptance Criteria

1. THE CSV_Sidecar SHALL use a schema version comment on the first line in the format `# schema_version=1.0` followed by the header row, to allow RaZStudio to detect format changes
2. THE CSV_Sidecar SHALL name columns identically to the field names defined in Requirement 2 and Requirement 6 acceptance criteria
3. THE GYRO_Block and ETTR_Block SHALL follow the standard MLV block convention (4-char blockType, mlv_hdr_t prefix) so that existing MLV parsers can skip unknown blocks gracefully
4. THE MLV_Recorder SHALL write GYRO_Block and ETTR_Block entries interleaved with video frame blocks in recording order (not batched at file end)

### Requirement 12: Graceful Degradation Without External Hardware

**User Story:** As a Canon 6D user without external gyro hardware, I want the system to function fully for all non-gyro metadata, so that I still get useful exposure/lens data in my sidecars and MLV files.

#### Acceptance Criteria

1. WHEN no External_IMU is connected and no IS lens vibration data is available, THE MOV_Metadata_Logger SHALL still create CSV sidecars with all non-gyro columns populated and gyro columns set to zero
2. WHEN no External_IMU is connected, THE MLV_Recorder SHALL omit GYRO_Block entries but still write ETTR_Block entries if ETTR is enabled
3. THE Gyro_Bridge SHALL initialize successfully and report status "disconnected" without producing errors or warnings that would alarm the user
4. THE MOV_Metadata_Logger SHALL include a comment row `# gyro_source=none|external_imu|is_lens` after the schema version line to indicate the active gyro data source
