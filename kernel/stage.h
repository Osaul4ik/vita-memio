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

/* DMA copy's state and counters (VM_KEY_DMACOPY). */
void vm_stage_dma(VmDma *out);

/* An app's own settings (the overlay; kept in its profile): each -1 for the
 * global one (Settings), else a value of that setting; thr: its thresholds
 * with read ahead On (cached, non-cacheable), 0 for the fixed one, else
 * 2..8 or VM_RA_NEVER. */
typedef struct VmApp {
	int32_t  ra, offload, pipe, gpusafe;
	uint32_t thr[2];
} VmApp;
#define VM_APP_GLOBAL  (-1)

/* What read ahead is set to now (the settings in effect), for the HUD. */
typedef struct VmRaNow {
	int      mode;          /* VM_RA_* in effect (global, or the app's)       */
	uint32_t thr[2];        /* in use: reads before it starts (VM_RA_NEVER)   */
	uint32_t set[2];        /* ... as set (the app's or the fixed): Adaptive
	                         * moves thr from there                            */
	uint32_t mid[2];        /* in use for the 32 KiB reads' stream (reads)    */
	uint32_t tiny;          /* in use for tiny reads: from which read a block */
	int      bufmap;        /* the app in front's buffers for the system core
	                         * (VM_BUFMAP_*), since it was started             */
} VmRaNow;

/* VmRaNow.bufmap: copy on the system core off or nothing mapped yet; every
 * buffer mapped; some mapped from their 2nd page (their first in no block:
 * the game core copies that part); some refused (the game core copies them). */
enum { VM_BUFMAP_NONE, VM_BUFMAP_OK, VM_BUFMAP_WORKAROUND, VM_BUFMAP_REFUSED };
void vm_stage_ra_now(VmRaNow *out);

/* The app in front's own settings, for the overlay. */
typedef struct VmAppNow {
	char  title[16];        /* "" : no app in front (the home screen)         */
	VmApp cur;              /* in effect now (applied, maybe not saved)       */
	VmApp saved;            /* as in its profile                              */
	VmApp global;           /* the global ones (Settings; thr: 0)             */
} VmAppNow;
void vm_stage_app_now(VmAppNow *out);
/* Worker thread. VM_OK, VM_ERR_NOT_READY (no app in front), VM_ERR_ARG (a
 * value out of range), VM_ERR_IO.
 *   set:     its settings, in effect at once (not saved);
 *   save:    as they are now, into its profile. */
int  vm_stage_app_set(const VmApp *a);
int  vm_stage_app_save(void);

/* Calibration: what read ahead brings, measured on this console, per storage
 * (a read from the pieces against the same read from the card; how long the
 * game's reads waited for a piece read for nothing). Measured only while on;
 * what was measured is kept (ur0:data/vita-memio/calib.bin) and used by
 * Adaptive and the R/A Test once there is enough of it, else the fixed
 * figures. */
typedef struct VmCal {
	int      on;            /* measuring                                      */
	int      have[2];       /* C, NC: enough measured, in use                 */
	int      have_waste;
	uint32_t n_plain[2], n_served[2], n_waste;   /* measured so far (kept)    */
	int32_t  gain16[2];     /* in use: per sector, sixteenths of a us         */
	int32_t  waste_us;      /* in use: a piece read for nothing               */
} VmCal;
#define VM_CAL_MIN_PLAIN   64u
#define VM_CAL_MIN_SERVED  64u
#define VM_CAL_MIN_WASTE   32u
void vm_stage_cal(int dev, VmCal *out);        /* dev -1: the fixed figures */
void vm_stage_cal_set(int on);                 /* worker thread             */
void vm_stage_cal_reset(void);                 /* worker: all measured gone */

/* The overlay's read ahead test. Worker thread, but for _running and _stale. */
typedef struct VmRaTestPick {
	int      little;        /* too little was read to tell: nothing picked    */
	int      ra;            /* VM_RA_*                                        */
	uint32_t thr[2];        /* C, NC: 2..8, 0 the fixed, VM_RA_NEVER: a loss  */
	int      offload;       /* VM_OFFLOAD_*                                   */
	uint32_t mib;           /* read during it                                 */
} VmRaTestPick;
int  vm_stage_ratest_start(void);              /* VM_OK, VM_ERR_NOT_READY */
int  vm_stage_ratest_running(uint32_t *mib);
int  vm_stage_ratest_stale(void);              /* another app came to the front */
void vm_stage_ratest_cancel(void);
void vm_stage_ratest_stop(VmRaTestPick *out);

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
/* Whether the HUD or the overlay shows transfers now, whatever the settings
 * say (either by keys): every transfer's bytes are counted while it does. */
void vm_stage_count_hud(int on);

/* Thread stacks. Measured on a console (the log's stack line: the least
 * each had free over a session with overlapped reads, read ahead, copy on
 * the system core and a buffer mapped from its 2nd page): worker 2.4 KB used,
 * the per-storage threads 1.3-1.6 KB. Half again or more is left free. */
#define VM_STACK_WORKER  0x1800
#define VM_STACK_DEV     0x1000

#endif /* VITAMEMIO_STAGE_H */
