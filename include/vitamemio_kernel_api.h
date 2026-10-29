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

/* Clear the crash-loop guard (safe mode). The plugin starts normally again at
 * the next boot; a boot already in safe mode stays inactive until then.
 * Returns VM_OK or VM_ERR_IO (the guard file could not be written). */
int vmResetSafeMode(void);

/* Round-trip cookie: proves the syscall path reaches the kernel module.
 * Always returns VM_SELFTEST_MAGIC; changes nothing. 'TST1'. */
#define VM_SELFTEST_MAGIC  0x54535431u
int vmSelfTestPing(void);

#ifdef __cplusplus
}
#endif

#endif /* VITAMEMIO_KERNEL_API_H */
