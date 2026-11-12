/*
 * hud_link.c - the kernel module's side of the HUD module (see hud_link.h):
 * loads vitamemio_hud.skprx while the HUD or the overlay needs it, hands it
 * the functions it needs, calls it from the worker, and unloads it again
 * when, with the overlay on, both are closed.
 */
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>

#include "hud_link.h"
#include "klog.h"
#include "overlay.h"
#include "overlay_logic.h"
#include "settings.h"
#include "stage.h"

/* Where the HUD module is looked for: next to the kernel module, which the
 * README puts in ur0:tai/ (ux0:tai/ for a config there). */
static const char *const k_paths[] = {
	"ur0:tai/vitamemio_hud.skprx",
	"ux0:tai/vitamemio_hud.skprx",
};

static void (*g_wake)(void);

static void core_wake(void)
{
	void (*const fn)(void) = __atomic_load_n(&g_wake, __ATOMIC_ACQUIRE);

	if (fn)
		fn();
}

static const VmCoreApi k_core = {
	.size           = sizeof(VmCoreApi),
	.build          = VM_BUILD_STR,
	.wake           = core_wake,
	.vlog           = vm_vklog,
	.settings_copy  = vm_settings_copy,
	.bytes          = vm_stage_bytes,
	.memcard_slow   = vm_stage_memcard_slow,
	.fg             = vm_stage_fg,
	.ra_now         = vm_stage_ra_now,
	.core_stats     = vm_stage_core_stats,
	.count_cores    = vm_stage_count_cores,
	.count_hud      = vm_stage_count_hud,
	.app_now        = vm_stage_app_now,
	.app_set        = vm_stage_app_set,
	.app_save       = vm_stage_app_save,
	.ratest_start   = vm_stage_ratest_start,
	.ratest_running = vm_stage_ratest_running,
	.ratest_stale   = vm_stage_ratest_stale,
	.ratest_cancel  = vm_stage_ratest_cancel,
	.ratest_stop    = vm_stage_ratest_stop,
	.ov_installed   = vm_overlay_installed,
	.ov_state       = vm_overlay_state,
	.ov_cursor      = vm_overlay_cursor,
	.ov_set_mask    = vm_overlay_set_mask,
	.ov_take_op     = vm_overlay_take_op,
};

static VmHudLink        g_link;
static VmHudLink *const g_link_ptr = &g_link;   /* module_start's argument */
static const VmHudApi  *g_hud;                   /* NULL: not loaded           */
static SceUID           g_modid = -1;            /* loaded (g_hud) or released */
static int              g_failed;                /* tried; again once both off */
static uint32_t         g_idle_us;               /* not needed since; 0: needed */
static uint32_t         g_released_us;           /* released at; 0: not        */

/* Closed this long (overlay on), the module's hook is released; this long
 * after that, the module is unloaded (any frame then in its hook is done:
 * a frame takes 17 ms). */
#define HUD_IDLE_US    3000000u
#define HUD_UNLOAD_US  1000000u

/* The load runs on a thread of its own: loading a module takes more stack
 * than the worker has. Its exit status is the module id or an error. */
static int load_thread(SceSize args, void *argp)
{
	(void)args; (void)argp;
	SceUID id = -1;

	for (unsigned i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]); i++) {
		int status = -1;

		id = ksceKernelLoadStartModule(k_paths[i], sizeof(g_link_ptr), (void *)&g_link_ptr,
		                               0, NULL, &status);
		if (id >= 0 && status == SCE_KERNEL_START_SUCCESS)
			return id;
		if (id >= 0)                      /* it did not stay (another build) */
			return (int)0x80010016;       /* SCE_ERROR_ERRNO_EINVAL           */
	}
	return id;
}

static void load(void)
{
	SceUID th;
	int id = -1;

	memset(&g_link, 0, sizeof(g_link));
	g_link.magic = VM_HUD_LINK_MAGIC;
	g_link.version = VM_HUD_LINK_VERSION;
	g_link.core = &k_core;
	th = ksceKernelCreateThread("SceVitaMemioHudLoad", load_thread, 0x80, 0x4000, 0, 0, NULL);
	if (th >= 0) {
		if (ksceKernelStartThread(th, 0, NULL) >= 0)
			ksceKernelWaitThreadEnd(th, &id, NULL);
		ksceKernelDeleteThread(th);
	}
	if (id >= 0 && g_link.hud.size == sizeof(VmHudApi) && g_link.hud.tick &&
	    g_link.hud.busy && g_link.hud.fini) {
		g_modid = id;
		__atomic_store_n(&g_hud, &g_link.hud, __ATOMIC_RELEASE);
		return;
	}
	g_failed = 1;
	VM_KLOGW("hud: vitamemio_hud.skprx not loaded (0x%08x): copy it next to "
	         "vitamemio_kernel.skprx, from the same release", (unsigned)id);
}

/* Its hook released: nothing calls into it any more, except a frame that
 * may still be drawing. */
static void release(void)
{
	const VmHudApi *const h = g_hud;

	__atomic_store_n(&g_hud, NULL, __ATOMIC_RELEASE);
	h->fini();
	g_released_us = ksceKernelGetSystemTimeLow() | 1u;
	VM_KLOGI("hud: closed, unloading the HUD module");
}

static void unload(void)
{
	int status;

	if (g_modid >= 0)
		ksceKernelStopUnloadModule(g_modid, 0, NULL, 0, NULL, &status);
	g_modid = -1;
	g_released_us = 0;
}

void vm_hud_link_tick(const VmDevState now[VM_DEV_COUNT], void (*wake)(void))
{
	const uint32_t t = ksceKernelGetSystemTimeLow();
	VmSettings s;
	int want, need;

	__atomic_store_n(&g_wake, wake, __ATOMIC_RELEASE);
	vm_settings_copy(&s);
	want = s.hud != VM_HUD_OFF || s.overlay;
	if (!want)
		g_failed = 0;                     /* switched on again: try again */
	/* the keys: with the setting, and only while the module can be loaded
	 * to draw what they open (else the game would lose its buttons to an
	 * overlay nobody sees) */
	vm_overlay_enable(s.overlay && !g_failed, core_wake);
	/* With the overlay on, the HUD shows only by its keys: the module is
	 * needed while they have something open, or a read ahead test runs. */
	need = want && (!s.overlay || !vm_overlay_installed() ||
	                vm_overlay_state() != OV_CLOSED || vm_stage_ratest_running(NULL));
	if (need) {
		g_idle_us = 0;
	} else if (g_hud) {
		if (!g_idle_us)
			g_idle_us = t | 1u;
		else if (t - g_idle_us >= HUD_IDLE_US)
			release();
	}
	if (g_released_us && !g_hud && t - g_released_us >= HUD_UNLOAD_US)
		unload();
	if (need && !g_hud && !g_released_us && !g_failed)
		load();
	if (g_hud)
		g_hud->tick(now);                 /* also when off: it hides them */
}

int vm_hud_link_busy(void)
{
	const VmHudApi *const h = __atomic_load_n(&g_hud, __ATOMIC_ACQUIRE);

	if (h) {
		const int b = h->busy();

		/* closed, but still loaded: a tick a second to unload it on time */
		return b == VM_HUD_IDLE && g_idle_us ? VM_HUD_SHOWN : b;
	}
	if (g_released_us)
		return VM_HUD_SHOWN;              /* to unload it on time */
	/* not loaded: the keys opened the overlay, load it at once */
	return vm_overlay_state() == OV_MENU ? VM_HUD_MENU :
	       vm_overlay_state() == OV_HUD ? VM_HUD_SHOWN : VM_HUD_IDLE;
}

void vm_hud_link_fini(void)
{
	vm_overlay_enable(0, NULL);          /* the keys' hooks out */
	if (g_hud) {
		const VmHudApi *const h = g_hud;

		__atomic_store_n(&g_hud, NULL, __ATOMIC_RELEASE);
		h->fini();
	}
	if (g_modid >= 0) {
		ksceKernelDelayThread(50000);     /* a frame still in its hook */
		unload();
	}
}
