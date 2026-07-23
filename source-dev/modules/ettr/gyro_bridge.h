/*
 * gyro_bridge.h — External IMU interface for Magic Lantern video metadata.
 *
 * Exposes per-frame angular velocity data from an external IMU connected via
 * the hot-shoe serial interface. Data is consumed by:
 *   - Lua scripts (mlc.gyro() → MOV CSV sidecars, Gyroflow logs)
 *   - MLV block writer (GYRO blocks embedded in RAW .MLV files)
 *
 * Static allocation throughout — no malloc. Single-writer (CBR_VSYNC poll),
 * multi-reader (Lua + MLV callback). 32-bit aligned int32 reads are atomic
 * on ARM Cortex-R4, so no mutex is required.
 *
 * External IMU Serial Protocol (UNVERIFIED — REQUIRES HARDWARE PROBING)
 * =====================================================================
 * Hot-shoe serial interface: bit-banged UART, 115200 baud, 8N1
 * STATUS: UNVERIFIED — Canon hot-shoe pinout is proprietary.
 *         Requires oscilloscope/logic analyzer probing of the EOS 6D
 *         hot-shoe contacts before implementation.
 *
 * Packet: 12 bytes
 *   [0..1]  gyroX  int16, big-endian, in mdps/10
 *   [2..3]  gyroY  int16, big-endian, in mdps/10
 *   [4..5]  gyroZ  int16, big-endian, in mdps/10
 *   [6..7]  accelX int16 (reserved, ignored)
 *   [8..9]  accelY int16 (reserved, ignored)
 *   [10..11] checksum: XOR of bytes 0..9
 *
 * Scale: multiply int16 value by 10 → mdps
 * Rate: IMU sends at >= 100 Hz (we poll 1/frame)
 * Pins: hot-shoe contact 4 = RX, contact 2 = GND
 *
 * If hot-shoe proves unusable, fallbacks:
 *   - USB-serial via camera's mini-USB port
 *   - SD-card-slot IMU module
 *   Both require separate investigation.
 */

#ifndef _gyro_bridge_h_
#define _gyro_bridge_h_

#include <stdint.h>

/* Forward declare lua_State to avoid requiring lua headers in all includers */
struct lua_State;
typedef struct lua_State lua_State;

/* Gyro reading in milli-degrees per second (mdps).
 * Valid range: -2,000,000 to +2,000,000 (±2000 deg/s full-scale). */
typedef struct {
    int32_t x;
    int32_t y;
    int32_t z;
} gyro_reading_t;

typedef enum {
    GYRO_STATUS_DISCONNECTED = 0,
    GYRO_STATUS_ACTIVE       = 1,
    GYRO_STATUS_TIMEOUT      = 2,
} gyro_status_t;

/* Initialize the gyro bridge. Probes serial interface.
 * Must be called once at module load. */
void gyro_bridge_init(void);

/* Poll the IMU. Called from CBR_VSYNC (once per frame).
 * Completes within 2ms (timeout) + conversion < 50µs total. */
void gyro_bridge_poll(void);

/* Get the latest reading (non-blocking, <50µs). */
gyro_reading_t gyro_bridge_get(void);

/* Get current status. */
gyro_status_t gyro_bridge_status(void);

/* Lua bindings: mlc.gyro() -> {x, y, z}, mlc.gyro_status() -> string */
int luaCB_gyro(lua_State *L);
int luaCB_gyro_status(lua_State *L);

/* Electronic level bindings: mlc.level() -> {pitch, roll} in degrees
 * Available on all EOS bodies with virtual horizon:
 *   Full-frame: 6D, 5D3, 5D4, 5DS/R, 1DX, 1DX2, 6D2
 *   APS-C: 7D2, 70D, 80D, 77D, 760D, 800D
 *   Mirrorless: M5
 * Reads Canon property PROP_ROLLING_PITCHING_LEVEL (0x80030039),
 * accelerometer-based, ~5 Hz update rate.
 * On bodies without this sensor, returns {0, 0}. */
int luaCB_level(lua_State *L);

#endif
