/*
 * stage.h - the faster storage path (kernel/stage.c).
 */
#ifndef VITAMEMIO_STAGE_H
#define VITAMEMIO_STAGE_H

#include "../include/vitamemio.h"
#include "../include/vitamemio_kernel_api.h"   /* VmTimingDev */

/* Allocate the per-device staging buffers and install the I/O hooks. All hooks
 * or none: on failure everything is released and storage stays stock. */
int  vm_stage_init(void);
void vm_stage_fini(void);

/* Apply settings (per-device on/off, verify mode). Safe from any context. */
void vm_stage_apply(const VmSettings *s);

/* Called once a second by the worker (allocates the verify buffer on demand). */
void vm_stage_worker_tick(void);

/* Called (from the hook, after the request is done with the buffers) when a
 * device has an error or a verify mismatch, so the worker can log it at once
 * instead of polling. NULL: nothing is called. */
void vm_stage_on_problem(void (*fn)(void));

int  vm_stage_verify_ready(void);
/* 1 while releasing a device's helper waits for a request still in flight. */
int  vm_stage_pending(void);

/* Memory card slow mode: 1 on, 0 off, -1 unknown. Read only. */
int  vm_stage_memcard_slow(void);

/* Bytes read/written on `dev` since boot, faster path or not (for the HUD). */
void vm_stage_bytes(int dev, uint64_t *rd, uint64_t *wr);

/* Live per-device status and counters. */
void vm_stage_get(VmDevState out[VM_DEV_COUNT], int plugin_active);

/* Where faster-path time goes: system read/write vs the CPU copy. */
void vm_stage_timing(VmTimingDev out[VM_DEV_COUNT]);

#endif /* VITAMEMIO_STAGE_H */
