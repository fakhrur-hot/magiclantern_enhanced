/* mlv_metadata.h — Custom MLV block extensions for gyro + ETTR metadata.
 *
 * Defines GYRO and ETTR block structures for embedding per-frame
 * metadata directly into MLV RAW video files. These blocks use the
 * standard mlv_hdr_t prefix so existing MLV parsers can skip them
 * gracefully.
 *
 * Validates: Requirements 7.2, 7.3, 7.4, 8.2, 8.3, 8.4
 */

#ifndef _mlv_metadata_h_
#define _mlv_metadata_h_

#include <raw.h>
#include "../raw_video/mlv_rec/mlv.h"
#include "../raw_video/mlv_rec/mlv_rec_interface.h"

/* Custom MLV block: GYRO
 *
 * Layout:
 *   Offset  Size  Field         Description
 *   0       4     blockType     "GYRO" (ASCII)
 *   4       4     blockSize     sizeof(mlv_hdr_t) + 16
 *   8       8     timestamp     us since recording start (set by mlv_rec)
 *   16      4     frameNumber   uint32, 0-indexed
 *   20      4     gyroX         int32, milli-degrees/sec
 *   24      4     gyroY         int32, milli-degrees/sec
 *   28      4     gyroZ         int32, milli-degrees/sec
 *                               Total: sizeof(mlv_hdr_t) + 16 bytes payload
 */
#pragma pack(push, 1)
typedef struct {
    mlv_hdr_t hdr;           /* blockType="GYRO", blockSize, timestamp */
    uint32_t  frameNumber;   /* 0-indexed frame counter */
    int32_t   gyroX;         /* milli-degrees/sec */
    int32_t   gyroY;         /* milli-degrees/sec */
    int32_t   gyroZ;         /* milli-degrees/sec */
} mlv_gyro_hdr_t;            /* sizeof = sizeof(mlv_hdr_t) + 16 */

/* Custom MLV block: ETTR
 *
 * Layout:
 *   Offset  Size  Field             Description
 *   0       4     blockType         "ETTR" (ASCII)
 *   4       4     blockSize         sizeof(mlv_hdr_t) + 16
 *   8       8     timestamp         us since recording start
 *   16      4     frameNumber       uint32, 0-indexed
 *   20      2     sceneDR           int16, 1/100 EV
 *   22      2     highlightHR       int16, highlight headroom, 1/100 EV
 *   24      2     clipR             uint16, 0-10000 (0.0-1.0 fraction)
 *   26      2     clipG             uint16, 0-10000
 *   28      2     clipB             uint16, 0-10000
 *   30      2     evBias            int16, 1/8 EV units
 *                                   Total: sizeof(mlv_hdr_t) + 16 bytes payload
 */
typedef struct {
    mlv_hdr_t hdr;           /* blockType="ETTR", blockSize, timestamp */
    uint32_t  frameNumber;   /* 0-indexed frame counter */
    int16_t   sceneDR;       /* 1/100 EV units */
    int16_t   highlightHR;   /* highlight headroom, 1/100 EV */
    uint16_t  clipR;         /* 0-10000 (0.0-1.0 fraction) */
    uint16_t  clipG;         /* 0-10000 */
    uint16_t  clipB;         /* 0-10000 */
    int16_t   evBias;        /* 1/8 EV units */
} mlv_ettr_hdr_t;            /* sizeof = sizeof(mlv_hdr_t) + 16 */
#pragma pack(pop)

/* Register the MLV recording callback. Call at module init.
 * Registers for MLV_REC_EVENT_VIDF -- on each video frame, builds
 * and enqueues GYRO and/or ETTR blocks via mlv_rec_queue_block(). */
void mlv_metadata_init(void);

#endif
