/*
 * mlv_metadata.c — MLV block writer for GYRO and ETTR per-frame metadata.
 *
 * Registers for MLV_REC_EVENT_VIDF and MLV_REC_EVENT_STARTED callbacks.
 * On each video frame (VIDF), conditionally builds and enqueues GYRO and/or
 * ETTR blocks via mlv_rec_queue_block() (non-blocking).
 *
 * Design constraints:
 *   - Static allocation only (no malloc). Pre-allocated block pool.
 *   - Non-blocking: mlv_rec_queue_block() handles write scheduling.
 *   - Frame counter resets at recording start (MLV_REC_EVENT_STARTED).
 *   - GYRO block emitted ONLY if gyro_bridge_status() == GYRO_STATUS_ACTIVE.
 *   - ETTR block emitted ONLY if auto_ettr != 0.
 *   - Blocks interleaved with video frames in recording order (not batched).
 *   - Graceful when mlv_rec module is not loaded (weak functions return 0).
 *
 * Validates: Requirements 7.1, 7.5, 7.6, 8.1, 8.5, 8.6, 9.4, 11.4
 */

#include <dryos.h>
#include <string.h>

#include "mlv_metadata.h"
#include "gyro_bridge.h"

/* Forward declarations from ettr.c / ai_lut.h — we only need the metadata
 * type, the getter, and the auto_ettr enable flag.  Including the full
 * ai_lut.h pulls in heavy dependencies (raw_info, histogram, lens_info)
 * that aren't available in this translation unit. */
extern int auto_ettr;
typedef struct { int light_level; float scene_dr; float highlight_headroom; float clip_r, clip_g, clip_b; } ettr_metadata_t;
ettr_metadata_t ettr_last_metadata(void);

/* ---------------------------------------------------------------------------
 * Static block pool
 *
 * We pre-allocate a small pool of GYRO and ETTR block instances. Each VIDF
 * callback uses one of each (round-robin). The pool must be large enough to
 * cover the pipeline depth of mlv_rec's write queue — 4 slots is generous
 * since blocks are small (32 bytes each) and written quickly.
 * ---------------------------------------------------------------------------
 */
#define MLV_META_POOL_SIZE  4

static mlv_gyro_hdr_t gyro_pool[MLV_META_POOL_SIZE];
static mlv_ettr_hdr_t ettr_pool[MLV_META_POOL_SIZE];

/* Round-robin indices into the pools */
static uint32_t gyro_pool_idx = 0;
static uint32_t ettr_pool_idx = 0;

/* Frame counter: 0-indexed, reset at recording start */
static uint32_t frame_counter = 0;

/* Sets a block's 4-byte type field directly, instead of calling mlv_rec's
 * mlv_set_type() (mlv.h). That function is a plain hard extern -- unlike
 * mlv_rec_register_cbr()/mlv_rec_queue_block() below, it is NOT declared
 * WEAK_FUNC, so depending on it would make this module fail to link with
 * "undefined symbol 'mlv_set_type'" whenever mlv_lite/mlv_rec isn't enabled
 * (e.g. mlv_lite.en absent), contradicting this file's own "graceful when
 * mlv_rec module is not loaded" claim. Our block types are always exactly
 * 4 ASCII characters ("GYRO"/"ETTR"), so mlv_set_type()'s string-length
 * handling isn't needed -- a direct 4-byte copy is simpler and dependency-free. */
static void set_block_type(mlv_hdr_t * hdr, const char * type)
{
    memcpy(hdr->blockType, type, 4);
}

/* ---------------------------------------------------------------------------
 * Callback: MLV_REC_EVENT_STARTED
 *
 * Resets the frame counter at the beginning of each recording session.
 * ---------------------------------------------------------------------------
 */
static void mlv_metadata_started_cbr(uint32_t event, void *ctx, mlv_hdr_t *hdr)
{
    (void)event;
    (void)ctx;
    (void)hdr;

    frame_counter = 0;
    gyro_pool_idx = 0;
    ettr_pool_idx = 0;
}

/* ---------------------------------------------------------------------------
 * Callback: MLV_REC_EVENT_VIDF
 *
 * Called from the EDMAC CBR context for every video frame queued for write.
 * Must be fast and non-blocking.
 *
 * Logic:
 *   1. If gyro_bridge_status() == GYRO_STATUS_ACTIVE → build & enqueue GYRO block
 *   2. If auto_ettr != 0 → build & enqueue ETTR block
 *   3. Increment frame counter
 * ---------------------------------------------------------------------------
 */
static void mlv_metadata_vidf_cbr(uint32_t event, void *ctx, mlv_hdr_t *hdr)
{
    (void)event;
    (void)ctx;
    (void)hdr;

    /* --- GYRO block (only if IMU is active) --- */
    if (gyro_bridge_status() == GYRO_STATUS_ACTIVE)
    {
        mlv_gyro_hdr_t *blk = &gyro_pool[gyro_pool_idx];
        gyro_pool_idx = (gyro_pool_idx + 1) % MLV_META_POOL_SIZE;

        /* Fill header */
        memset(blk, 0, sizeof(*blk));
        set_block_type(&blk->hdr, "GYRO");
        blk->hdr.blockSize = sizeof(mlv_gyro_hdr_t);

        /* Fill payload */
        blk->frameNumber = frame_counter;
        gyro_reading_t g = gyro_bridge_get();
        blk->gyroX = g.x;
        blk->gyroY = g.y;
        blk->gyroZ = g.z;

        /* Enqueue (non-blocking; timestamp set by mlv_rec) */
        mlv_rec_queue_block(&blk->hdr);
    }

    /* --- ETTR block (only if ETTR is enabled) --- */
    if (auto_ettr)
    {
        mlv_ettr_hdr_t *blk = &ettr_pool[ettr_pool_idx];
        ettr_pool_idx = (ettr_pool_idx + 1) % MLV_META_POOL_SIZE;

        /* Fill header */
        memset(blk, 0, sizeof(*blk));
        set_block_type(&blk->hdr, "ETTR");
        blk->hdr.blockSize = sizeof(mlv_ettr_hdr_t);

        /* Fill payload from ETTR module's last computed metadata */
        blk->frameNumber = frame_counter;
        ettr_metadata_t m = ettr_last_metadata();

        /* Convert float values to fixed-point integers:
         *   sceneDR:      float EV → int16 in 1/100 EV
         *   highlightHR:  float EV → int16 in 1/100 EV
         *   clip channels: float 0.0–1.0 → uint16 0–10000
         *   evBias:       stored as-is (1/8 EV units from ETTR engine) */
        blk->sceneDR     = (int16_t)(m.scene_dr * 100);
        blk->highlightHR = (int16_t)(m.highlight_headroom * 100);
        blk->clipR       = (uint16_t)(m.clip_r * 10000);
        blk->clipG       = (uint16_t)(m.clip_g * 10000);
        blk->clipB       = (uint16_t)(m.clip_b * 10000);
        blk->evBias      = 0;  /* Reserved: no per-frame bias exposed yet */

        /* Enqueue (non-blocking; timestamp set by mlv_rec) */
        mlv_rec_queue_block(&blk->hdr);
    }

    /* Advance frame counter for next VIDF */
    frame_counter++;
}

/* ---------------------------------------------------------------------------
 * mlv_metadata_init — Register MLV recording callbacks.
 *
 * Call once at module load (from ettr module_init). If mlv_rec is not loaded,
 * mlv_rec_register_cbr is a weak stub returning 0 — safe to call regardless.
 * ---------------------------------------------------------------------------
 */
void mlv_metadata_init(void)
{
    /* Reset state */
    frame_counter = 0;
    gyro_pool_idx = 0;
    ettr_pool_idx = 0;

    /* Register for STARTED event to reset frame counter each recording */
    mlv_rec_register_cbr(MLV_REC_EVENT_STARTED, mlv_metadata_started_cbr, NULL);

    /* Register for VIDF event to emit per-frame metadata blocks */
    mlv_rec_register_cbr(MLV_REC_EVENT_VIDF, mlv_metadata_vidf_cbr, NULL);
}
