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
/* Read ahead's measuring (auto or statistics): the worker's next tick in
 * microseconds, 0 if only when woken. */
unsigned vm_stage_tick_us(void);

/* Memory card slow mode: 1 on, 0 off, -1 unknown. Read only. */
int  vm_stage_memcard_slow(void);

/* Bytes read/written on `dev` since boot, faster path or not (for the HUD). */
void vm_stage_bytes(int dev, uint64_t *rd, uint64_t *wr);

/* Live per-device status and counters. */
void vm_stage_get(VmDevState out[VM_DEV_COUNT], int plugin_active);

/* Where faster-path time goes: system read/write vs the CPU copy. */
void vm_stage_timing(VmTimingDev out[VM_DEV_COUNT]);

/* Read ahead "Auto" profiles (kernel buffers; any thread). */
int  vm_stage_ra_list(VmRaProfile *out, int max);
int  vm_stage_ra_delete(const char *title);

/* An app's own settings (the overlay; kept in its profile): each -1 for the
 * global one (Settings), else a value of that setting; thr: its thresholds
 * with read ahead On (cached, non-cacheable), 0 for the fixed one, else
 * 2..8 or VM_RA_NEVER. */
typedef struct VmApp {
	int32_t  ra, offload, pipe, gpusafe;
	uint32_t thr[2];
} VmApp;
#define VM_APP_GLOBAL  (-1)

/* What read ahead is doing now (the settings in effect), for the HUD and
 * the overlay. */
typedef struct VmRaNow {
	char     title[16];     /* the game Auto follows ("" none)               */
	int      mode;          /* VM_RA_* in effect (global, or the app's)       */
	int      measure;       /* following an app (Auto, or Full statistics)    */
	int      learning;      /* 1 while Auto learns it                         */
	uint32_t streams;       /* ... streams seen so far                        */
	uint32_t target;        /* ... of this many (a game 300, homebrew 150)    */
	uint32_t minutes;       /* ... minutes in front so far                    */
	uint32_t percent;       /* ... how far (streams or time), 0..99           */
	uint32_t thr[2];        /* in use: reads before it starts (VM_RA_NEVER)   */
	int      dev;           /* its storage, -1 not known                      */
} VmRaNow;
void vm_stage_ra_now(VmRaNow *out);

/* The app in front's own settings, for the overlay. */
typedef struct VmAppNow {
	char  title[16];        /* "" : no app in front (the home screen)         */
	VmApp cur;              /* in effect now (applied, maybe not saved)       */
	VmApp saved;            /* as in its profile                              */
	VmApp global;           /* the global ones (Settings; thr: 0)             */
	int   learnt;           /* Auto has learnt it                             */
} VmAppNow;
void vm_stage_app_now(VmAppNow *out);
/* Worker thread. VM_OK, VM_ERR_NOT_READY (no app in front), VM_ERR_ARG (a
 * value out of range), VM_ERR_IO.
 *   set:     its settings, in effect at once (not saved);
 *   save:    as they are now, into its profile;
 *   relearn: what Auto learnt of it forgotten (its settings kept). */
int  vm_stage_app_set(const VmApp *a);
int  vm_stage_app_save(void);
int  vm_stage_app_relearn(void);

/* The app in front (process events) and the storage it reads from most
 * since it came to the front (-1: none in front, or too little read yet;
 * counted while the HUD or Statistics counts transfers). */
typedef struct VmFg {
	char title[16];
	int  dev;
} VmFg;
void vm_stage_fg(VmFg *out);


/* Where reads were copied / read, since boot (KiB; wrap-safe differences):
 * the copies by core, and non-cacheable reads on the system core vs the
 * caller's own. Counted while vm_stage_count_cores(1) or the log is on. */
void vm_stage_core_stats(uint32_t cp_core_kib[4], uint32_t *nc_sys_kib, uint32_t *nc_own_kib);
void vm_stage_count_cores(int on);

#endif /* VITAMEMIO_STAGE_H */
