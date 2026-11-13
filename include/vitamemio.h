/*
 * vitamemio.h - shared constants and types for vita-memio (PS Vita
 * storage plugin).
 *
 * Firmware 3.60-3.74 CEX, taiHEN/HENkaku. Both modules include this header, so
 * every struct here that crosses the syscall boundary is POD and must be built
 * from this one definition.
 *
 * Layout of the project:
 *   kernel/        vitamemio_kernel.skprx - the storage I/O hooks (staging),
 *                  settings persistence, the worker, the boot guard, and
 *                  the user-callable "SceVitaMemio" syscall library.
 *   user/          vitamemio.suprx entry point (loaded only in the Settings
 *                  app, *NPXS10015).
 *   settings_app/  the vita-memio page under Settings > Devices; it talks to
 *                  the kernel module through the syscalls.
 */
#ifndef VITAMEMIO_H
#define VITAMEMIO_H

#include <stdint.h>

/* Release version. The .yml module versions carry major.minor (0.2); bump all
 * three together. Logged in both modules' init banner so a log pins the build. */
#define VM_VERSION_STR    "1.3.0"

/* Which build: "+" and the commit it was built from (CMakeLists.txt sets it
 * at configure time; empty when built outside a git checkout). The version
 * alone does not tell two builds of one version apart. */
#ifndef VM_BUILD_SUFFIX
#define VM_BUILD_SUFFIX   ""
#endif
#define VM_BUILD_STR      VM_VERSION_STR VM_BUILD_SUFFIX

/* Everything the plugin writes lives here (created on first use). */
#define VM_DATA_DIR       "ur0:data/vita-memio"

/* Error codes returned across the whole plugin. */
#define VM_OK              0
#define VM_ERR_NOT_READY  -1
#define VM_ERR_RESOLVE    -2   /* module/export not found                    */
#define VM_ERR_MISMATCH   -3   /* unknown firmware: nothing installed        */
#define VM_ERR_ARG        -4   /* bad pointer, key id or value               */
#define VM_ERR_IO         -5
#define VM_ERR_NOMEM      -6   /* staging buffer could not be allocated      */

/* ------------------------------------------------------------------------- */
/* Devices                                                                   */
/* ------------------------------------------------------------------------- */

enum vm_dev {
	VM_DEV_SD2VITA = 0,   /* microSD in the game-card slot (SD2Vita adapter) */
	VM_DEV_INTERNAL,      /* reserved: the built-in storage is never sped up */
	VM_DEV_MEMCARD,       /* the official PS Vita memory card                */
	VM_DEV_COUNT
};

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */
/* ------------------------------------------------------------------------- */

/* Key ids for vmSetSetting(). The order is also the field order of VmSettings
 * and the registry key table in the Settings page. Slots 4 and 5 were the HUD
 * corner and the developer mode in settings files of version 1; such a file
 * loads with both at their defaults (kernel/settings.c). */
enum vm_key {
	VM_KEY_SD2VITA = 0,    /* 0/1: faster SD2Vita reads and writes           */
	VM_KEY_INTERNAL,       /* reserved, always 0: was faster internal storage */
	VM_KEY_VERIFY,         /* 0/1: check every faster transfer (diagnostic)  */
	VM_KEY_HUD,            /* reserved: the HUD is always Extended (VM_HUD_*) */
	VM_KEY_HUD_STYLE,      /* reserved: the HUD has one look now             */
	VM_KEY_LOG,            /* 0/1: write kernel.log / user.log at all        */
	VM_KEY_MEMCARD,        /* 0/1: faster memory card (added after 0.1.0)    */
	VM_KEY_SMALL,          /* 0/1: small transfers (< 32 KiB) faster too     */
	VM_KEY_STATS,          /* VM_STATS_*: timers and counters */
	VM_KEY_GPUSAFE,        /* VM_GPUSAFE_*: write read data out of the CPU cache */
	VM_KEY_PIPELINE,       /* VM_PIPE_*: overlapped reads                    */
	VM_KEY_OFFLOAD,        /* VM_OFFLOAD_*: copy on the system core          */
	VM_KEY_READAHEAD,      /* VM_RA_*: read ahead while a stream is read              */
	VM_KEY_OVERLAY,        /* 0/1: the SELECT + arrows overlay (and HUD by keys) */
	VM_KEY_DMACOPY,        /* 0/1: copy reads out with the DMA controller    */
	VM_KEY_COUNT
};

#define VM_HUD_OFF        0
#define VM_HUD_COMPACT    1
#define VM_HUD_EXTENDED   2

/* Overlapped reads: off; auto (parts sized from the measured costs, 2 or 3
 * of them, whichever is quicker); fixed (2 parts, 3 with GPU-safe reads,
 * sized from built-in costs, nothing measured). 1 was "on" before the
 * choice existed and means auto. */
#define VM_PIPE_OFF       0
#define VM_PIPE_AUTO      1
#define VM_PIPE_FIXED     2

/* Copy on the system core: a game's read is copied out of the staging
 * buffers by a thread on the system core instead of the game's own thread
 * (whose core is then free sooner): off; only with GPU-safe reads (where the
 * copy costs the most); always. */
#define VM_OFFLOAD_OFF     0
#define VM_OFFLOAD_ALWAYS  2   /* "Cached only" (1: the removed "GPU-safe only")     */
#define VM_OFFLOAD_ALL     3   /* "C + NC R/A": and what read ahead holds for NC     */
#define VM_OFFLOAD_FULL    4   /* "C + NC All": and non-cacheable reads themselves   */

/* Which reads a VM_OFFLOAD_* setting puts on the system core: reads into
 * ordinary (cached) memory, and
 * reads into non-cacheable memory (incl. read-ahead pieces copied into it). */
static inline int vm_offload_cached(int v)
{
	return v == VM_OFFLOAD_ALWAYS || v == VM_OFFLOAD_ALL || v == VM_OFFLOAD_FULL;
}
static inline int vm_offload_nc(int v)
{
	return v == VM_OFFLOAD_ALL || v == VM_OFFLOAD_FULL;
}
/* ... and whether non-cacheable reads themselves go there, straight in. */
static inline int vm_offload_direct(int v)
{
	return v == VM_OFFLOAD_FULL;
}

/* GPU-safe reads: after a faster read, its data is written out of the CPU
 * cache to memory, so the graphics chip sees it: never; after every copy
 * except those the DMA controller made (VM_KEY_DMACOPY), whose data it wrote
 * itself; after every copy. 1 was "on" before the choice existed and stays
 * "all". */
#define VM_GPUSAFE_OFF     0
#define VM_GPUSAFE_ALL     1
#define VM_GPUSAFE_NODMA   2   /* "Non-DMA" */

/* Statistics: off; on (per-transfer timers and data counters, for the tester). */
#define VM_STATS_OFF       0
#define VM_STATS_ON        1

/* Read ahead: off; on (it starts after a fixed number of reads in a row: the
 * app's, from the R/A Test, or the standard one); adaptive (from that number,
 * moved by what it brings: started sooner while what it reads is used, later
 * while much is read for nothing; never off). */
#define VM_RA_OFF          0
#define VM_RA_ON           1
#define VM_RA_ADAPTIVE     2

/* The HUD always sits in the top right corner. */
#define VM_HUD_STYLE_CLASSIC   0   /* white text with a shadow, 8x8 font x2     */
#define VM_HUD_STYLE_PSVSHELL  1   /* a black panel, Terminus font, colours     */

/* Defaults. The internal storage (which holds the system's own data) is never
 * sped up: its key stays, always 0, so settings files keep their layout. */
#define VM_DEF_SD2VITA   0   /* opt-in: see README, "use it when you need it" */
#define VM_DEF_INTERNAL  0
#define VM_DEF_VERIFY    0
#define VM_DEF_HUD       VM_HUD_EXTENDED   /* shown by SELECT + Right with the overlay */
#define VM_DEF_HUD_STYLE VM_HUD_STYLE_PSVSHELL
#define VM_DEF_LOG       0   /* logging is opt-in (Settings > Advanced)        */
#define VM_DEF_MEMCARD   0
#define VM_DEF_SMALL     1   /* on: small transfers are file-system bookkeeping */
#define VM_DEF_STATS     0   /* off; the tester's timings need it switched on */
#define VM_DEF_GPUSAFE   VM_GPUSAFE_NODMA /* clean after CPU copies; DMA writes memory */
#define VM_DEF_PIPELINE  1   /* VM_PIPE_AUTO: overlapped reads tune themselves */
#define VM_DEF_OFFLOAD   VM_OFFLOAD_ALL   /* "C + NC R/A"; without read ahead as Cached only */
#define VM_DEF_READAHEAD 2   /* VM_RA_ADAPTIVE                                 */
#define VM_DEF_OVERLAY   1   /* on: it takes SELECT + arrows from games        */
#define VM_DEF_DMACOPY   1   /* on; stops itself on errors or a mismatch       */

/* One int per key, in enum vm_key order. */
typedef struct VmSettings {
	int32_t sd2vita;
	int32_t internal;
	int32_t verify;
	int32_t hud;
	int32_t hud_style;
	int32_t log;
	int32_t memcard;
	int32_t small;
	int32_t stats;
	int32_t gpusafe;
	int32_t pipeline;
	int32_t offload;
	int32_t readahead;
	int32_t overlay;
	int32_t dmacopy;
} VmSettings;

/* The first seven keys, as VmState carries them (VmState is frozen, so keys
 * added later are read with vmGetSetting). */
typedef struct VmStateSettings {
	int32_t sd2vita;
	int32_t internal;
	int32_t verify;
	int32_t hud;
	int32_t hud_style;
	int32_t log;
	int32_t memcard;
} VmStateSettings;

/* 1 if `v` is a legal value for `key`, else 0. Shared so the kernel's
 * validation and the Settings page agree. */
static inline int vm_setting_valid(int key, int v)
{
	switch (key) {
	case VM_KEY_INTERNAL:
		return v == 0;
	case VM_KEY_SD2VITA:
	case VM_KEY_VERIFY:
	case VM_KEY_MEMCARD:
	case VM_KEY_LOG:
	case VM_KEY_SMALL:
	case VM_KEY_OVERLAY:
	case VM_KEY_DMACOPY:
		return v == 0 || v == 1;
	case VM_KEY_GPUSAFE:
		return v >= VM_GPUSAFE_OFF && v <= VM_GPUSAFE_NODMA;
	case VM_KEY_READAHEAD:
		return v >= VM_RA_OFF && v <= VM_RA_ADAPTIVE;
	case VM_KEY_STATS:
		return v == VM_STATS_OFF || v == VM_STATS_ON;
	case VM_KEY_PIPELINE:
		return v >= VM_PIPE_OFF && v <= VM_PIPE_FIXED;
	case VM_KEY_OFFLOAD:
		return v == VM_OFFLOAD_OFF || (v >= VM_OFFLOAD_ALWAYS && v <= VM_OFFLOAD_FULL);
	case VM_KEY_HUD:                 /* not in Settings: always Extended */
		return v == VM_HUD_EXTENDED;
	case VM_KEY_HUD_STYLE:
		return v == VM_HUD_STYLE_CLASSIC || v == VM_HUD_STYLE_PSVSHELL;
	default:
		return 0;
	}
}

_Static_assert(sizeof(VmSettings) == VM_KEY_COUNT * sizeof(int32_t),
               "VmSettings: one int32 per key, in enum vm_key order");

static inline int32_t *vm_setting_field(VmSettings *s, int key)
{
	if (key < 0 || key >= VM_KEY_COUNT)
		return 0;
	return &((int32_t *)s)[key];   /* the struct is VM_KEY_COUNT int32s       */
}

/* ------------------------------------------------------------------------- */
/* Live status                                                               */
/* ------------------------------------------------------------------------- */

/* VmDevState.status: what a device is doing right now. */
#define VM_ST_OFF        0   /* turned off in Settings                         */
#define VM_ST_ACTIVE     1   /* on: large transfers take the faster path       */
#define VM_ST_FALLBACK   2   /* switched itself back to stock after an error   */
#define VM_ST_NO_BUFFER  3   /* its staging buffer could not be allocated      */
#define VM_ST_UNAVAIL    4   /* plugin inactive (see VmState.disabled)         */

typedef struct VmDevState {
	int32_t  status;          /* VM_ST_*                                       */
	uint32_t reads;           /* transfers that took the faster path           */
	uint32_t writes;
	uint32_t read_mib;        /* data moved on the faster path, MiB            */
	uint32_t write_mib;
	uint32_t passthru;        /* large transfers left stock (buffer busy)      */
	uint32_t errors;          /* transfers that failed on the faster path      */
	uint32_t verified;        /* transfers checked in verify mode              */
	uint32_t mismatches;      /* checks that failed (caller got stock data)    */
	uint32_t time_ms;         /* unused, 0 (kept: the struct is frozen)        */
	uint32_t max_ms;          /* unused, 0                                     */
} VmDevState;

#endif /* VITAMEMIO_H */
