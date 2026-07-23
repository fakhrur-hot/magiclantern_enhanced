/*
 * gyro_bridge.c — External IMU interface for Magic Lantern video metadata.
 *
 * Polls an external IMU connected via hot-shoe serial at frame cadence,
 * maintains the latest angular velocity reading in a static buffer, and
 * exposes it to Lua scripts and the MLV block writer.
 *
 * HARDWARE STATUS: UNVERIFIED
 * ===========================
 * The serial read is stubbed (always returns 0 / no data). The full
 * implementation requires hardware probing of the EOS 6D hot-shoe contacts
 * with an oscilloscope/logic analyzer. Until then, the state machine runs
 * but never transitions to ACTIVE — it settles at DISCONNECTED.
 *
 * State machine:
 *   [init] → PROBING (probe for 100ms)
 *   PROBING → DISCONNECTED (no response in 100ms)
 *   PROBING → ACTIVE (IMU responds — stub never reaches this)
 *   ACTIVE → TIMEOUT (single 2ms poll timeout)
 *   TIMEOUT → ACTIVE (next poll succeeds)
 *   TIMEOUT → DISCONNECTED (10 consecutive timeouts)
 *   DISCONNECTED → ACTIVE (IMU detected on next poll)
 *
 * Memory: All static allocation, no malloc/calloc/realloc.
 * Threading: Single-writer (CBR_VSYNC), multi-reader (Lua + MLV).
 *            32-bit aligned int32 reads are atomic on ARM Cortex-R4.
 *
 * Validates: Requirements 3.2, 3.4, 3.5, 3.7, 4.1, 4.2, 4.3, 4.4
 */

#include "gyro_bridge.h"
#include <dryos.h>
#include <stdint.h>
#include <string.h>
#include "../lua/lua_common.h"   /* Lua API: lua_State, lua_newtable, etc. */

/* ──────────────────────────────────────────────────────────────────────────
 * Constants
 * ────────────────────────────────────────────────────────────────────────── */

#define GYRO_TIMEOUT_MS          2    /* Max wait per poll attempt           */
#define GYRO_PROBE_MS          100    /* Init probe window                   */
#define GYRO_TIMEOUT_LIMIT      10    /* Consecutive timeouts → disconnected */
#define GYRO_PACKET_LEN         12    /* IMU packet size in bytes            */
#define GYRO_MDPS_MAX      2000000    /* ±2000 deg/s full-scale clamp        */
#define GYRO_MDPS_MIN     -2000000

/* ──────────────────────────────────────────────────────────────────────────
 * Static state (no dynamic allocation)
 * ────────────────────────────────────────────────────────────────────────── */

static gyro_reading_t  gyro_current;       /* Latest reading (zeros if no IMU)  */
static gyro_reading_t  gyro_last_valid;    /* Last successful reading for hold  */
static gyro_status_t   gyro_state;         /* Current state machine state       */
static int             timeout_counter;    /* Consecutive timeout count (0..10) */

/* ──────────────────────────────────────────────────────────────────────────
 * Stubbed serial interface
 * ────────────────────────────────────────────────────────────────────────── */

/* STUB: Always returns 0 (no data available).
 * Replace with actual hot-shoe GPIO UART read once hardware is verified. */
static int gyro_serial_read(uint8_t *buf, int len)
{
    (void)buf; (void)len;
    return 0;  /* no data available */
}

/* ──────────────────────────────────────────────────────────────────────────
 * IMU packet parsing (ready for when hardware works)
 * ────────────────────────────────────────────────────────────────────────── */

/* Clamp a value to the valid mdps range. */
static int32_t gyro_clamp(int32_t val)
{
    if (val > GYRO_MDPS_MAX) return GYRO_MDPS_MAX;
    if (val < GYRO_MDPS_MIN) return GYRO_MDPS_MIN;
    return val;
}

/* Validate checksum: XOR of bytes 0..9 must equal bytes 10..11.
 * Returns 1 if valid, 0 if invalid. */
static int gyro_validate_checksum(const uint8_t *pkt)
{
    uint8_t xor_hi = 0;
    uint8_t xor_lo = 0;
    int i;

    /* XOR all payload bytes, alternating into hi/lo accumulators */
    for (i = 0; i < 10; i += 2)
    {
        xor_hi ^= pkt[i];
        xor_lo ^= pkt[i + 1];
    }

    return (xor_hi == pkt[10]) && (xor_lo == pkt[11]);
}

/* Parse a 12-byte IMU packet into a gyro_reading_t.
 * Raw int16 big-endian values are scaled ×10 to get mdps, then clamped.
 * Returns 1 on success, 0 on checksum failure. */
static int gyro_parse_packet(const uint8_t *pkt, gyro_reading_t *out)
{
    int16_t raw_x, raw_y, raw_z;

    if (!gyro_validate_checksum(pkt))
        return 0;

    /* int16 big-endian extraction */
    raw_x = (int16_t)((pkt[0] << 8) | pkt[1]);
    raw_y = (int16_t)((pkt[2] << 8) | pkt[3]);
    raw_z = (int16_t)((pkt[4] << 8) | pkt[5]);

    /* Scale: raw value × 10 → milli-degrees per second */
    out->x = gyro_clamp((int32_t)raw_x * 10);
    out->y = gyro_clamp((int32_t)raw_y * 10);
    out->z = gyro_clamp((int32_t)raw_z * 10);

    return 1;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Attempt a single serial read with timeout.
 * Returns 1 if a valid packet was received, 0 otherwise.
 * ────────────────────────────────────────────────────────────────────────── */
static int gyro_attempt_read(gyro_reading_t *out)
{
    uint8_t buf[GYRO_PACKET_LEN];
    int bytes_read;

    /* Try to read a full packet from the serial interface.
     * The 2ms timeout is enforced by the caller's polling cadence —
     * gyro_serial_read is non-blocking (stub returns immediately). */
    bytes_read = gyro_serial_read(buf, GYRO_PACKET_LEN);

    if (bytes_read < GYRO_PACKET_LEN)
        return 0;  /* No data or incomplete packet */

    return gyro_parse_packet(buf, out);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API
 * ────────────────────────────────────────────────────────────────────────── */

/*
 * gyro_bridge_init — Initialize the gyro bridge.
 *
 * Probes the serial interface for 100ms. If no IMU responds (stub: always),
 * settles to DISCONNECTED. Zeroes the reading buffers.
 *
 * Must be called once at module load time.
 */
void gyro_bridge_init(void)
{
    gyro_reading_t probe_result;

    /* Zero all static state */
    memset(&gyro_current, 0, sizeof(gyro_current));
    memset(&gyro_last_valid, 0, sizeof(gyro_last_valid));
    timeout_counter = 0;

    /* Enter probing state */
    gyro_state = GYRO_STATUS_TIMEOUT;  /* Temporarily use TIMEOUT during probe */

    /* Probe: attempt to read from serial interface.
     * Give the IMU 100ms to respond (per spec: probe window). */
    gyro_state = GYRO_STATUS_DISCONNECTED; /* Assume disconnected before probe */

    if (gyro_attempt_read(&probe_result))
    {
        /* IMU responded during probe — go active */
        gyro_current = probe_result;
        gyro_last_valid = probe_result;
        gyro_state = GYRO_STATUS_ACTIVE;
        timeout_counter = 0;
    }
    else
    {
        /* No response — wait the full probe window, then confirm disconnected.
         * STUB: gyro_serial_read always returns 0, so we always land here. */
        msleep(GYRO_PROBE_MS);

        /* Second attempt after settling time */
        if (gyro_attempt_read(&probe_result))
        {
            gyro_current = probe_result;
            gyro_last_valid = probe_result;
            gyro_state = GYRO_STATUS_ACTIVE;
            timeout_counter = 0;
        }
        else
        {
            /* Confirmed: no IMU present */
            gyro_state = GYRO_STATUS_DISCONNECTED;
            timeout_counter = 0;
        }
    }
}

/*
 * gyro_bridge_poll — Poll the IMU once per frame.
 *
 * Called from CBR_VSYNC at frame cadence (~24–60 Hz depending on video mode).
 * Implements the state machine:
 *   - ACTIVE/TIMEOUT: attempt read; on failure increment timeout_counter
 *   - 10 consecutive timeouts → DISCONNECTED
 *   - On success: reset counter, update current reading, go ACTIVE
 *   - DISCONNECTED: attempt read in case IMU reconnects
 *
 * When timed out, retains gyro_last_valid in gyro_current (or zeros if
 * we never had a valid reading).
 *
 * STUB: Since hardware is unverified, gyro_serial_read always returns 0.
 *       The state machine logic is complete but never transitions to ACTIVE
 *       after init. It will remain DISCONNECTED until hardware is verified.
 */
void gyro_bridge_poll(void)
{
    gyro_reading_t new_reading;

    switch (gyro_state)
    {
        case GYRO_STATUS_ACTIVE:
        case GYRO_STATUS_TIMEOUT:
            /* Attempt to read from the IMU */
            if (gyro_attempt_read(&new_reading))
            {
                /* Success: update readings, reset timeout counter */
                gyro_current = new_reading;
                gyro_last_valid = new_reading;
                gyro_state = GYRO_STATUS_ACTIVE;
                timeout_counter = 0;
            }
            else
            {
                /* Timeout: retain last valid reading */
                gyro_current = gyro_last_valid;
                gyro_state = GYRO_STATUS_TIMEOUT;
                timeout_counter++;

                /* 10 consecutive timeouts → transition to disconnected */
                if (timeout_counter >= GYRO_TIMEOUT_LIMIT)
                {
                    gyro_state = GYRO_STATUS_DISCONNECTED;
                    timeout_counter = 0;
                    /* Zero the current reading since IMU is gone */
                    memset(&gyro_current, 0, sizeof(gyro_current));
                }
            }
            break;

        case GYRO_STATUS_DISCONNECTED:
            /* Periodically check if IMU reconnects */
            if (gyro_attempt_read(&new_reading))
            {
                /* IMU is back! */
                gyro_current = new_reading;
                gyro_last_valid = new_reading;
                gyro_state = GYRO_STATUS_ACTIVE;
                timeout_counter = 0;
            }
            /* Otherwise remain disconnected, gyro_current stays zeroed */
            break;
    }
}

/*
 * gyro_bridge_get — Get the latest gyro reading.
 *
 * Non-blocking, <50µs. Simply returns the static buffer.
 * Safe for multi-reader access: 32-bit aligned int32 reads are atomic on ARM.
 */
gyro_reading_t gyro_bridge_get(void)
{
    return gyro_current;
}

/*
 * gyro_bridge_status — Get the current IMU connection status.
 *
 * Returns: GYRO_STATUS_DISCONNECTED, GYRO_STATUS_ACTIVE, or GYRO_STATUS_TIMEOUT.
 */
gyro_status_t gyro_bridge_status(void)
{
    return gyro_state;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Electronic Level Data (from PROP_ROLLING_PITCHING_LEVEL)
 * ────────────────────────────────────────────────────────────────────────── */

/* Electronic level data from Canon's accelerometer sensor.
 * Updated by property handler at ~5 Hz (registered in MODULE_PROPHANDLERS).
 * Available on: 6D, 5D3, 5D4, 7D2, 70D, 80D, etc.
 *
 * Encoding (from Canon property 0x80030039):
 *   roll  = roll_sensor1 * 256 + roll_sensor2   (0.01° units, unsigned)
 *   pitch = pitch_sensor1 * 256 + pitch_sensor2 (0.01° units, unsigned)
 *   Values > 18000 represent negative angles (wrap at 36000 = 360°).
 *   status == 2 means sensor is active and data is valid.
 */
static struct {
    uint8_t status;
    uint8_t cameraposture;
    uint8_t roll_sensor1;
    uint8_t roll_sensor2;
    uint8_t pitch_sensor1;
    uint8_t pitch_sensor2;
} gyro_level_data;

/* Property handler callback — called by ML property system.
 * Declared here, registered via MODULE_PROPHANDLERS in ettr.c (task 2.4). */
void gyro_level_prop_handler(unsigned int property, void *buf, unsigned int len)
{
    if (len >= 6)
        memcpy(&gyro_level_data, buf, 6);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Lua Bindings
 *
 * Validates: Requirements 3.1, 3.4, 4.3, 6.4
 * ────────────────────────────────────────────────────────────────────────── */

/*
 * luaCB_gyro — mlc.gyro() → {x=int32, y=int32, z=int32}
 *
 * Returns a Lua table with the latest gyro reading in milli-degrees/sec.
 * Non-blocking, < 50µs (just reads static buffer).
 * Returns {0, 0, 0} when no IMU is connected.
 *
 * Validates: Requirement 3.1, 3.4
 */
int luaCB_gyro(lua_State *L)
{
    gyro_reading_t r = gyro_bridge_get();

    lua_newtable(L);
    lua_pushinteger(L, r.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, r.y);
    lua_setfield(L, -2, "y");
    lua_pushinteger(L, r.z);
    lua_setfield(L, -2, "z");

    return 1;  /* one return value (the table) */
}

/*
 * luaCB_gyro_status — mlc.gyro_status() → "active"|"timeout"|"disconnected"
 *
 * Returns the current IMU connection state as a string.
 *
 * Validates: Requirement 4.3
 */
int luaCB_gyro_status(lua_State *L)
{
    gyro_status_t st = gyro_bridge_status();

    switch (st)
    {
        case GYRO_STATUS_ACTIVE:
            lua_pushstring(L, "active");
            break;
        case GYRO_STATUS_TIMEOUT:
            lua_pushstring(L, "timeout");
            break;
        case GYRO_STATUS_DISCONNECTED:
        default:
            lua_pushstring(L, "disconnected");
            break;
    }

    return 1;  /* one return value (the string) */
}

/*
 * luaCB_level — mlc.level() → {pitch=float, roll=float}
 *
 * Returns the Canon electronic level sensor values in degrees.
 * Available on EOS bodies with virtual horizon (6D, 5D3, 5D4, 7D2, etc.).
 * Returns {0, 0} on bodies without the sensor or if data is invalid.
 *
 * The data comes from PROP_ROLLING_PITCHING_LEVEL (0x80030039), which
 * Canon updates at ~5 Hz from the camera's accelerometer.
 *
 * Value encoding (from Canon property):
 *   roll  = (roll_sensor1 * 256 + roll_sensor2)  → 0.01° units
 *   pitch = (pitch_sensor1 * 256 + pitch_sensor2) → 0.01° units
 *   Values > 18000 are negative angles (wraps at 36000).
 *   Converted to degrees by dividing by 100.
 *
 * Validates: Requirement 6.4
 */
int luaCB_level(lua_State *L)
{
    /* Decode Canon's packed format: two bytes → 0.01° value */
    int roll100 = (int)(gyro_level_data.roll_sensor1) * 256
                + (int)(gyro_level_data.roll_sensor2);
    int pitch100 = (int)(gyro_level_data.pitch_sensor1) * 256
                 + (int)(gyro_level_data.pitch_sensor2);

    /* Wrap to signed range: values > 18000 are negative angles */
    if (roll100 > 18000)  roll100  -= 36000;
    if (pitch100 > 18000) pitch100 -= 36000;

    /* Convert to degrees (floating point OK here — Lua side only,
     * not on the shared integer-only code path) */
    float roll_deg  = (float)roll100  / 100.0f;
    float pitch_deg = (float)pitch100 / 100.0f;

    /* Check for invalid data (status != 2 means sensor not active) */
    if (gyro_level_data.status != 2)
    {
        roll_deg = 0.0f;
        pitch_deg = 0.0f;
    }

    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)pitch_deg);
    lua_setfield(L, -2, "pitch");
    lua_pushnumber(L, (lua_Number)roll_deg);
    lua_setfield(L, -2, "roll");

    return 1;  /* one return value (the table) */
}
