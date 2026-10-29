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
 *                  settings persistence, the 1 Hz worker, the boot guard, and
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
#define VM_VERSION_STR    "0.2.0-alpha"

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
	VM_DEV_INTERNAL,      /* the console's built-in storage (ur0:, imc0:...) */
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
	VM_KEY_INTERNAL,       /* 0/1: faster internal storage                   */
	VM_KEY_VERIFY,         /* 0/1: check every faster transfer (diagnostic)  */
	VM_KEY_HUD,            /* VM_HUD_*                                       */
	VM_KEY_HUD_STYLE,      /* VM_HUD_STYLE_*: how the HUD is drawn           */
	VM_KEY_LOG,            /* 0/1: write kernel.log / user.log at all        */
	VM_KEY_MEMCARD,        /* 0/1: faster memory card (added after 0.1.0)    */
	VM_KEY_COUNT
};

#define VM_HUD_OFF        0
#define VM_HUD_COMPACT    1
#define VM_HUD_EXTENDED   2

/* The HUD always sits in the top right corner. */
#define VM_HUD_STYLE_CLASSIC   0   /* white text with a shadow, 8x8 font x2     */
#define VM_HUD_STYLE_PSVSHELL  1   /* a black panel, Terminus font, colours     */

/* Defaults: the SD2Vita, the case the plugin is for, starts on; internal
 * storage (which also holds the system's own data) starts off. */
#define VM_DEF_SD2VITA   0   /* opt-in: see README, "use it when you need it" */
#define VM_DEF_INTERNAL  0
#define VM_DEF_VERIFY    0
#define VM_DEF_HUD       VM_HUD_OFF
#define VM_DEF_HUD_STYLE VM_HUD_STYLE_PSVSHELL
#define VM_DEF_LOG       0   /* logging is opt-in (Settings > Advanced)        */
#define VM_DEF_MEMCARD   0

/* One int per key, in enum vm_key order. */
typedef struct VmSettings {
	int32_t sd2vita;
	int32_t internal;
	int32_t verify;
	int32_t hud;
	int32_t hud_style;
	int32_t log;
	int32_t memcard;
} VmSettings;

/* 1 if `v` is a legal value for `key`, else 0. Shared so the kernel's
 * validation and the Settings page agree. */
static inline int vm_setting_valid(int key, int v)
{
	switch (key) {
	case VM_KEY_SD2VITA:
	case VM_KEY_INTERNAL:
	case VM_KEY_VERIFY:
	case VM_KEY_MEMCARD:
	case VM_KEY_LOG:
		return v == 0 || v == 1;
	case VM_KEY_HUD:
		return v >= VM_HUD_OFF && v <= VM_HUD_EXTENDED;
	case VM_KEY_HUD_STYLE:
		return v == VM_HUD_STYLE_CLASSIC || v == VM_HUD_STYLE_PSVSHELL;
	default:
		return 0;
	}
}

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
	uint32_t time_ms;         /* total time inside faster-path transfers, ms   */
	uint32_t max_ms;          /* the slowest single faster-path transfer, ms   */
} VmDevState;

#endif /* VITAMEMIO_H */
