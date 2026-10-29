/*
 * stage.h - the faster storage path (kernel/stage.c).
 */
#ifndef STORAGETURBO_STAGE_H
#define STORAGETURBO_STAGE_H

#include "../include/storageturbo.h"
#include "../include/storageturbo_kernel_api.h"   /* TsTimingDev */

/* Allocate the per-device staging buffers and install the I/O hooks. All hooks
 * or none: on failure everything is released and storage stays stock. */
int  ts_stage_init(void);
void ts_stage_fini(void);

/* Apply settings (per-device on/off, verify mode). Safe from any context. */
void ts_stage_apply(const TsSettings *s);

/* Called once a second by the worker (allocates the verify buffer on demand). */
void ts_stage_worker_tick(void);

int  ts_stage_verify_ready(void);

/* Memory card slow mode: 1 on, 0 off, -1 unknown. Read only. */
int  ts_stage_memcard_slow(void);

/* Bytes read/written on `dev` since boot, faster path or not (for the HUD). */
void ts_stage_bytes(int dev, uint64_t *rd, uint64_t *wr);

/* Live per-device status and counters. */
void ts_stage_get(TsDevState out[TS_DEV_COUNT], int plugin_active);

/* The experimental cached read path of a device: TS_CR_*, or TS_ERR_ARG. */
int  ts_stage_cached_state(int dev);

/* Where faster-path time goes: system read/write vs the CPU copy. */
void ts_stage_timing(TsTimingDev out[TS_DEV_COUNT]);

#endif /* STORAGETURBO_STAGE_H */
