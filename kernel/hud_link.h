/*
 * hud_link.h - how the kernel module and the HUD module talk.
 *
 * vitamemio_kernel.skprx (the storage path, settings, boot guard, log) and
 * vitamemio_hud.skprx (the HUD, the SELECT + arrows overlay and its read ahead
 * test menu) are two kernel modules. Only the first is in config.txt. It loads
 * the second itself, from the same folder, at the start when the overlay is
 * on (the HUD is the overlay's), so with it off none of the HUD's code, fonts
 * or buffers are in kernel memory. The module and its hooks are never put in
 * or taken out while apps run (hooks released under running apps crashed the
 * console): switched off, the overlay stops acting at once; switched on, it
 * acts only if it was on at the start, else from the next start. The HUD
 * module sees the overlay as it is in effect (VmCoreApi.settings_copy).
 *
 * No exported library and no NIDs: the kernel module passes a pointer to a
 * VmHudLink as module_start's argument; the HUD module checks it, keeps the
 * kernel module's functions (`core`) and fills in its own (`hud`). Both are
 * built together; the HUD module refuses a kernel module of another build.
 *
 * Every call into `hud` is made by the kernel module's worker thread.
 */
#ifndef VITAMEMIO_HUD_LINK_H
#define VITAMEMIO_HUD_LINK_H

#include <stdarg.h>
#include <stdint.h>

#include "../include/vitamemio.h"
#include "stage.h"

#define VM_HUD_LINK_MAGIC    0x4C44554Du   /* "MUDL" */
#define VM_HUD_LINK_VERSION  1

/* The kernel module's functions, for the HUD module. */
typedef struct VmCoreApi {
	uint32_t    size;                     /* sizeof(VmCoreApi)                  */
	const char *build;                    /* VM_BUILD_STR of the kernel module  */
	void (*wake)(void);                   /* wake the worker (a key was pressed) */
	void (*vlog)(int level, const char *fmt, va_list ap);
	void (*settings_copy)(VmSettings *out);
	void (*bytes)(int dev, uint64_t *rd, uint64_t *wr);
	int  (*memcard_slow)(void);
	void (*fg)(VmFg *out);
	void (*ra_now)(VmRaNow *out);
	void (*core_stats)(uint32_t cp_core_kib[4], uint32_t *nc_sys_kib, uint32_t *nc_own_kib);
	void (*count_cores)(int on);
	void (*count_hud)(int on);
	void (*app_now)(VmAppNow *out);
	int  (*app_set)(const VmApp *a);
	int  (*app_save)(void);
	int  (*ratest_start)(void);
	int  (*ratest_running)(uint32_t *mib);
	int  (*ratest_stale)(void);
	void (*ratest_cancel)(void);
	void (*ratest_stop)(VmRaTestPick *out);
} VmCoreApi;

/* What the worker should wake for, from VmHudApi.busy. */
#define VM_HUD_IDLE   0
#define VM_HUD_SHOWN  1   /* the HUD is on screen: every second            */
#define VM_HUD_MENU   2   /* the overlay is open: every 250 ms             */

/* The HUD module's functions, for the kernel module's worker. */
typedef struct VmHudApi {
	uint32_t size;                        /* sizeof(VmHudApi); 0: not filled in */
	/* The HUD's and the overlay's text, their hooks as the settings say. */
	void (*tick)(const VmDevState now[VM_DEV_COUNT]);
	int  (*busy)(void);                   /* VM_HUD_*                           */
	void (*fini)(void);                   /* every hook released                */
} VmHudApi;

typedef struct VmHudLink {
	uint32_t         magic;               /* VM_HUD_LINK_MAGIC                  */
	uint32_t         version;             /* VM_HUD_LINK_VERSION                */
	const VmCoreApi *core;
	VmHudApi         hud;                 /* filled in by the HUD module        */
} VmHudLink;

/* ---- the kernel module's side (kernel/hud_link.c), worker thread only ---- */

/* Load the HUD module if the overlay was on at the start (once), then let it
 * draw. */
void vm_hud_link_tick(const VmDevState now[VM_DEV_COUNT], void (*wake)(void));
int  vm_hud_link_busy(void);           /* VM_HUD_*; VM_HUD_IDLE without it   */
void vm_hud_link_fini(void);           /* module_stop: its hooks out, unloaded */

#endif /* VITAMEMIO_HUD_LINK_H */
