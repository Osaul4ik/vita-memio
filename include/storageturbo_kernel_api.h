/*
 * storageturbo_kernel_api.h - the "SceStorageTurbo" syscall library the kernel
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
 * Every function here must also be listed in storageturbo_kernel.yml and
 * defined in kernel/main.c.
 */
#ifndef STORAGETURBO_KERNEL_API_H
#define STORAGETURBO_KERNEL_API_H

#include "storageturbo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot of everything the kernel module knows, for the Settings page and
 * diagnostics. */
typedef struct TsState {
	uint32_t   size;             /* sizeof(TsState), set by the kernel        */
	uint32_t   magic;            /* TS_STATE_MAGIC                            */
	TsSettings settings;         /* current (applied) settings                */
	int32_t    disabled;         /* TS_DIS_*: why nothing is active           */
	uint32_t   fingerprint;      /* storage driver module fingerprint         */
	int32_t    verify_ready;     /* 1 once the verify buffer is allocated     */
	uint32_t   uptime_s;         /* seconds since the kernel module started   */
	int32_t    memcard_slow;     /* memory card slow mode: 1 on, 0 off, -1 n/a */
	TsDevState dev[TS_DEV_COUNT];
} TsState;

/* TsState.disabled */
#define TS_DIS_NONE      0   /* active                                          */
#define TS_DIS_FIRMWARE  1   /* unknown firmware: nothing installed, all stock  */
#define TS_DIS_HOOKS     2   /* hooks could not be installed: all stock         */
#define TS_DIS_SAFE      3   /* last start never finished: safe mode, all stock */

#define TS_STATE_MAGIC  0x54535354u   /* 'TSST' */

/* Copy the current state to *out (user pointer). Returns TS_OK or
 * TS_ERR_ARG. */
int tsGetState(TsState *out);

/* Apply one setting (enum ts_key) and save it. Returns TS_OK, or TS_ERR_ARG
 * for an unknown key or an out-of-range value (nothing changes then). */
int tsSetSetting(int key, int value);

/* Where the time of faster-path transfers goes, per device: inside the system's
 * own sector read/write ("io") and in the CPU copy between the caller's buffer
 * and the staging buffer ("copy"). Counted per chunk that succeeded; verify-mode
 * re-reads are not included. The values only grow and wrap at 2^32: use the
 * difference of two readings (unsigned subtraction is wrap-safe).
 *
 * A separate call with its own struct, so TsState keeps its size: a user module
 * built before this was added still gets exactly the TsState it expects. */
typedef struct TsTimingDev {
	uint32_t rd_sectors;      /* sectors read on the faster path               */
	uint32_t rd_io_us;        /* time inside the system read, us               */
	uint32_t rd_copy_us;      /* staging -> caller copy, us                    */
	uint32_t wr_sectors;
	uint32_t wr_io_us;        /* time inside the system write, us              */
	uint32_t wr_copy_us;      /* caller -> staging copy, us                    */
} TsTimingDev;

typedef struct TsTiming {
	uint32_t    size;         /* sizeof(TsTiming), set by the kernel           */
	uint32_t    magic;        /* TS_TIMING_MAGIC                               */
	TsTimingDev dev[TS_DEV_COUNT];
} TsTiming;

#define TS_TIMING_MAGIC  0x54535449u   /* 'TSTI' */

/* Copy the timing counters to *out (user pointer). Returns TS_OK or TS_ERR_ARG. */
int tsGetTiming(TsTiming *out);

/* Round-trip cookie: proves the syscall path reaches the kernel module.
 * Always returns TS_SELFTEST_MAGIC; changes nothing. 'TST1'. */
#define TS_SELFTEST_MAGIC  0x54535431u
int tsSelfTestPing(void);

#ifdef __cplusplus
}
#endif

#endif /* STORAGETURBO_KERNEL_API_H */
