/*
 * vitamemio_kernel_api.h - the "SceVitaMemio" syscall library the kernel
 * module publishes for the Settings page (or any other user module) to import.
 *
 * Why a kernel module at all: the storage I/O path runs in the kernel, and the
 * kernel module also owns the settings, so they apply from boot without any user
 * module running.
 *
 * These functions cross the user->kernel boundary: pointer arguments are copied
 * out with ksceKernelMemcpyKernelToUser inside the kernel, and every key id and
 * value is validated there. Keep the structs small and POD.
 *
 * Every function here must also be listed in vitamemio_kernel.yml and
 * defined in kernel/main.c.
 */
#ifndef VITAMEMIO_KERNEL_API_H
#define VITAMEMIO_KERNEL_API_H

#include "vitamemio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot of everything the kernel module knows, for the Settings page and
 * diagnostics. */
typedef struct VmState {
	uint32_t   size;             /* sizeof(VmState), set by the kernel        */
	uint32_t   magic;            /* VM_STATE_MAGIC                            */
	VmStateSettings settings;    /* current settings, the first seven keys    */
	int32_t    disabled;         /* VM_DIS_*: why nothing is active           */
	uint32_t   fingerprint;      /* storage driver module fingerprint         */
	int32_t    verify_ready;     /* 1 once the verify buffer is allocated     */
	uint32_t   uptime_s;         /* seconds since the kernel module started   */
	int32_t    memcard_slow;     /* memory card slow mode: 1 on, 0 off, -1 n/a */
	VmDevState dev[VM_DEV_COUNT];
} VmState;

/* VmState.disabled */
#define VM_DIS_NONE      0   /* active                                          */
#define VM_DIS_FIRMWARE  1   /* unknown firmware: nothing installed, all stock  */
#define VM_DIS_HOOKS     2   /* hooks could not be installed: all stock         */
#define VM_DIS_SAFE      3   /* last start never finished: safe mode, all stock */

#define VM_STATE_MAGIC  0x54535354u   /* 'TSST' */

/* Copy the current state to *out (user pointer). Returns VM_OK or
 * VM_ERR_ARG. */
int vmGetState(VmState *out);

/* Apply one setting (enum vm_key) and save it. Returns VM_OK, or VM_ERR_ARG
 * for an unknown key or an out-of-range value (nothing changes then). */
int vmSetSetting(int key, int value);

/* Where the time of faster-path transfers goes, per device: inside the system's
 * own sector read/write ("io") and in the CPU copy between the caller's buffer
 * and the staging buffer ("copy"). Counted per chunk that succeeded; verify-mode
 * re-reads are not included. The values only grow and wrap at 2^32: use the
 * difference of two readings (unsigned subtraction is wrap-safe).
 *
 * A separate call with its own struct, so VmState keeps its size: a user module
 * built before this was added still gets exactly the VmState it expects. */
typedef struct VmTimingDev {
	uint32_t rd_sectors;      /* sectors read on the faster path               */
	uint32_t rd_io_us;        /* time inside the system read, us               */
	uint32_t rd_copy_us;      /* staging -> caller copy, us                    */
	uint32_t wr_sectors;
	uint32_t wr_io_us;        /* time inside the system write, us              */
	uint32_t wr_copy_us;      /* caller -> staging copy, us                    */
} VmTimingDev;

typedef struct VmTiming {
	uint32_t    size;         /* sizeof(VmTiming), set by the kernel           */
	uint32_t    magic;        /* VM_TIMING_MAGIC                               */
	VmTimingDev dev[VM_DEV_COUNT];
} VmTiming;

#define VM_TIMING_MAGIC  0x54535449u   /* 'TSTI' */

/* FROZEN: vmGetState and vmGetTiming copy sizeof() of these into the caller's
 * struct, so a user module built against another header must get exactly the
 * size it expects. Never grow them: add new data through new calls. */
_Static_assert(sizeof(VmState) == 188, "VmState is frozen (its size since 0.1.0)");
_Static_assert(sizeof(VmTiming) == 80, "VmTiming is frozen (its size since tester 0.3)");

/* Copy the timing counters to *out (user pointer). Returns VM_OK or VM_ERR_ARG. */
int vmGetTiming(VmTiming *out);

/* The current value of one setting (enum vm_key), or VM_ERR_ARG for an
 * unknown key. Every key, including those VmState does not carry. */
int vmGetSetting(int key);

/* Which build the kernel module is (VM_BUILD_STR, e.g. "0.8.1+96bf8fb"), as a
 * string of at most `size` bytes with its terminator (user pointer). Returns
 * VM_OK or VM_ERR_ARG. Newer than 0.8.0: an older module does not have it. */
int vmGetBuild(char *out, int size);

/* Clear the crash-loop guard (safe mode). The plugin starts normally again at
 * the next boot; a boot already in safe mode stays inactive until then.
 * Returns VM_OK or VM_ERR_IO (the guard file could not be written). */
int vmResetSafeMode(void);

/* DMA copy (VM_KEY_DMACOPY): reads copied out of the staging
 * buffer by the console's DMA controller (the system's ksceDmacMemcpy) instead
 * of the CPU, for copies of whole cache lines from VM_DMA_MIN_BYTES up. Any
 * copy it refuses is made by the CPU instead; after VM_DMA_MAX_ERRORS refusals,
 * or one verify-mode difference while it is on, it stops until the setting is
 * switched off and on. Counters only grow and wrap. */
#define VM_DMA_MIN_BYTES   (16u * 1024u)   /* the smallest part of an overlapped read */
#define VM_DMA_ALIGN       64u        /* the Cortex-A9's cache line */
#define VM_DMA_MAX_ERRORS  8u

#define VM_DMA_OFF         0   /* the setting is off                              */
#define VM_DMA_ON          1   /* in use                                          */
#define VM_DMA_UNAVAIL     2   /* this system has no ksceDmacMemcpy to call        */
#define VM_DMA_STOPPED     3   /* stopped itself: see stop_why                     */

#define VM_DMA_WHY_NONE      0
#define VM_DMA_WHY_ERRORS    1   /* VM_DMA_MAX_ERRORS refusals (last_err)          */
#define VM_DMA_WHY_MISMATCH  2   /* verify mode found a difference                 */

typedef struct VmDma {
	uint32_t size;           /* sizeof(VmDma), set by the kernel                 */
	uint32_t magic;          /* VM_DMA_MAGIC                                     */
	int32_t  state;          /* VM_DMA_*                                         */
	int32_t  stop_why;       /* VM_DMA_WHY_*                                     */
	uint32_t copies;         /* copies the DMA controller made                   */
	uint32_t kib;            /* ... their data, KiB                              */
	uint32_t us;             /* ... time in ksceDmacMemcpy, us                   */
	uint32_t mid;            /* ... of them under 2 x VM_DMA_MIN_BYTES (16-32    */
	uint32_t mid_us;         /* KiB: the parts of overlapped reads), and their us */
	/* Copies left to the CPU while it is on (counted only then): */
	uint32_t small;          /* under VM_DMA_MIN_BYTES                           */
	uint32_t small_kib;      /* ... their data, KiB                              */
	uint32_t small_half;     /* ... of them from VM_DMA_MIN_BYTES / 2 up         */
	uint32_t unaligned;      /* large enough, but not whole cache lines (the     */
	                         /* caller's buffer)                                 */
	uint32_t unaligned_kib;  /* ... their data, KiB                              */
	uint32_t errors;         /* copies it refused (the CPU made them)            */
	int32_t  last_err;       /* the last refusal's error code, 0 if none         */
} VmDma;

#define VM_DMA_MAGIC  0x544D4441u   /* 'TMDA' */
_Static_assert(sizeof(VmDma) == 64, "VmDma: grow only through a new call");

/* Copy the DMA copy state to *out (user pointer). Returns VM_OK or
 * VM_ERR_ARG. Newer than 1.3.0: an older module does not have it. */
int vmGetDma(VmDma *out);

/* An app's threshold in the overlay: read ahead never starts for it. */
#define VM_RA_NEVER      0xFFFF

/* Round-trip cookie: proves the syscall path reaches the kernel module.
 * Always returns VM_SELFTEST_MAGIC; changes nothing. 'TST1'. */
#define VM_SELFTEST_MAGIC  0x54535431u
int vmSelfTestPing(void);

#ifdef __cplusplus
}
#endif

#endif /* VITAMEMIO_KERNEL_API_H */
