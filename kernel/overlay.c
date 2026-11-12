/*
 * overlay.c - the SELECT + arrows overlay's keys (see overlay_logic.h).
 *
 * The buttons are seen where every app and game reads them: the SceCtrl
 * syscalls (sceCtrlPeek/ReadBufferPositive/Negative, ...2, ...Ext, ...Ext2),
 * hooked in the kernel. After the original fills the caller's buffers, the
 * newest one is looked at; while the overlay is open the buttons in all of
 * them are cleared (sticks left alone), so the game does not act on them.
 * Nothing else is changed. With the overlay off at the start the hooks are
 * not there; switched off later they stay, doing nothing (hud_link.c).
 */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/sysmem.h>
#include <psp2kern/kernel/threadmgr.h>
#include <psp2kern/ctrl.h>
#include <taihen.h>
#include <stddef.h>

#include "overlay.h"
#include "overlay_logic.h"
#include "klog.h"
#include "vm_hook.h"

/* The user-callable SceCtrl functions (library SceCtrl). Negative ones carry
 * the buttons inverted. */
static const struct {
	uint32_t nid;
	int      negative;
} k_fn[] = {
	{ 0xA9C3CED6, 0 },   /* sceCtrlPeekBufferPositive     */
	{ 0x67E7AB83, 0 },   /* sceCtrlReadBufferPositive     */
	{ 0x104ED1A7, 1 },   /* sceCtrlPeekBufferNegative     */
	{ 0x15F96FB0, 1 },   /* sceCtrlReadBufferNegative     */
	{ 0x15F81E8C, 0 },   /* sceCtrlPeekBufferPositive2    */
	{ 0xC4226A3E, 0 },   /* sceCtrlReadBufferPositive2    */
	{ 0xA59454D3, 0 },   /* sceCtrlPeekBufferPositiveExt  */
	{ 0xE2D99296, 0 },   /* sceCtrlReadBufferPositiveExt  */
	{ 0x860BF292, 0 },   /* sceCtrlPeekBufferPositiveExt2 */
	{ 0xA7178860, 0 },   /* sceCtrlReadBufferPositiveExt2 */
};
#define OV_FNS ((int)(sizeof(k_fn) / sizeof(k_fn[0])))

static SceUID         g_hook[OV_FNS];
static volatile tai_hook_ref_t g_ref[OV_FNS];
static int            g_installed;      /* the hooks are in (worker only)   */
static volatile int   g_active;         /* ... and act: the setting is on   */
#define LD_ACTIVE()   __atomic_load_n(&g_active, __ATOMIC_ACQUIRE)

static OvLogic        g_ov;
static volatile int   g_lock;
static uint32_t       g_look_us;        /* closed: the last look (atomic)      */
static uint32_t       g_mask = OV_MASK_ALL;   /* the items shown (the worker's) */
static void (*volatile g_wake)(void);

/* The buttons of the newest buffer seen, through the logic; then, if the
 * overlay is open, cleared in all `count` of them (else those still held from
 * when it closed). */
static void on_pad(int port, SceCtrlData *pad, int count, int negative)
{
	uint32_t b = 0, mask;
	int changed, hide;

	if (!pad || count <= 0 || count > 64)
		return;
	/* closed: a look every OV_SAMPLE_US is enough (a press lasts longer), the
	 * HUD shown every OV_HUD_SAMPLE_US; the polls in between cost a clock
	 * read. Every poll while something is to be hidden. */
	const uint32_t now = ksceKernelGetSystemTimeLow();
	const int st = __atomic_load_n(&g_ov.state, __ATOMIC_RELAXED);

	if (st != OV_MENU && !__atomic_load_n(&g_ov.hide, __ATOMIC_RELAXED)) {
		if (now - __atomic_load_n(&g_look_us, __ATOMIC_RELAXED) <
		    (st == OV_HUD ? OV_HUD_SAMPLE_US : OV_SAMPLE_US))
			return;
		__atomic_store_n(&g_look_us, now, __ATOMIC_RELAXED);
	}
	if (ksceKernelMemcpyUserToKernel(&b, (const char *)&pad[count - 1] +
	                                 offsetof(SceCtrlData, buttons), sizeof(b)) < 0)
		return;
	if (negative)
		b = ~b;
	if (__sync_lock_test_and_set(&g_lock, 1)) {
		/* another thread is at it: its look counts; the hiding still applies */
		changed = 0;
		hide = __atomic_load_n(&g_ov.state, __ATOMIC_RELAXED) == OV_MENU;
		mask = __atomic_load_n(&g_ov.hide, __ATOMIC_RELAXED);
	} else {
		changed = ov_input(&g_ov, ov_merge(&g_ov, port, b), now,
		                   __atomic_load_n(&g_mask, __ATOMIC_RELAXED));
		hide = g_ov.state == OV_MENU;
		mask = g_ov.hide;
		__sync_lock_release(&g_lock);
	}
	if (changed) {
		void (*const fn)(void) = __atomic_load_n(&g_wake, __ATOMIC_ACQUIRE);

		if (fn)
			fn();
	}
	if (hide) {
		const uint32_t none = negative ? 0xFFFFFFFFu : 0;

		for (int i = 0; i < count; i++)
			ksceKernelMemcpyKernelToUser((char *)&pad[i] + offsetof(SceCtrlData, buttons),
			                             &none, sizeof(none));
	} else if (mask) {
		for (int i = 0; i < count; i++) {
			char *const at = (char *)&pad[i] + offsetof(SceCtrlData, buttons);
			uint32_t v;

			if (ksceKernelMemcpyUserToKernel(&v, at, sizeof(v)) < 0)
				break;
			v = negative ? v | mask : v & ~mask;
			ksceKernelMemcpyKernelToUser(at, &v, sizeof(v));
		}
	}
}

#define OV_HOOK(i)                                                               \
	static int ov_fn##i(int port, SceCtrlData *pad, int count)                   \
	{                                                                            \
		const tai_hook_ref_t ref = vm_hook_ref(&g_ref[i]);                       \
		int r = ref ? TAI_CONTINUE(int, ref, port, pad, count) : VM_HOOK_NO_REF; \
		if (r > 0 && LD_ACTIVE())                                                \
			on_pad(port, pad, r < count ? r : count, k_fn[i].negative);          \
		return r;                                                                \
	}
OV_HOOK(0) OV_HOOK(1) OV_HOOK(2) OV_HOOK(3) OV_HOOK(4)
OV_HOOK(5) OV_HOOK(6) OV_HOOK(7) OV_HOOK(8) OV_HOOK(9)

static const void *const k_hooks[OV_FNS] = {
	ov_fn0, ov_fn1, ov_fn2, ov_fn3, ov_fn4, ov_fn5, ov_fn6, ov_fn7, ov_fn8, ov_fn9,
};

/* The hooks are put in once, at the start with the overlay on, and stay
 * until the module stops: switching the setting off only stops them acting
 * (g_active). Taking them out while games call the SceCtrl functions from
 * several threads, many times a frame, rebooted the console (Overlay
 * switched off in Settings while it was showing): a call already past its
 * hook's entry can still read the released hook. Left in, an inactive hook
 * costs one load per call. */
static void hooks_install(void)
{
	int ok = 0;

	for (int i = 0; i < OV_FNS; i++) {
		g_ref[i] = 0;                  /* not the last time's: see vm_hook_ref */
		g_hook[i] = taiHookFunctionExportForKernel(KERNEL_PID, (tai_hook_ref_t *)&g_ref[i],
		                                           "SceCtrl", TAI_ANY_LIBRARY, k_fn[i].nid,
		                                           k_hooks[i]);
		ok += g_hook[i] >= 0;
	}
	g_installed = 1;
	if (ok == OV_FNS)
		VM_KLOGI("overlay: ready (SELECT + arrows)");
	else
		VM_KLOGW("overlay: %d of %d button hooks installed", ok, OV_FNS);
}

static void state_reset(void)
{
	while (__sync_lock_test_and_set(&g_lock, 1))
		ksceKernelDelayThread(100);
	memset(&g_ov, 0, sizeof(g_ov));       /* OV_CLOSED, nothing hidden */
	__sync_lock_release(&g_lock);
}

void vm_overlay_enable(int on, void (*wake)(void))
{
	__atomic_store_n(&g_wake, wake, __ATOMIC_RELEASE);
	if (on && !LD_ACTIVE()) {
		state_reset();                    /* afresh: nothing of an earlier time */
		if (!g_installed)
			hooks_install();
		__atomic_store_n(&g_active, 1, __ATOMIC_RELEASE);
	} else if (!on && LD_ACTIVE()) {
		__atomic_store_n(&g_active, 0, __ATOMIC_RELEASE);
		state_reset();                    /* closed: the game gets its buttons */
	}
}

void vm_overlay_fini(void)
{
	__atomic_store_n(&g_active, 0, __ATOMIC_RELEASE);
	if (!g_installed)
		return;
	for (int i = 0; i < OV_FNS; i++)
		if (g_hook[i] >= 0) {
			taiHookReleaseForKernel(g_hook[i], g_ref[i]);
			g_hook[i] = -1;
		}
	g_installed = 0;
	state_reset();
}

int vm_overlay_installed(void)
{
	return LD_ACTIVE();
}

int vm_overlay_state(void)
{
	return LD_ACTIVE() ? __atomic_load_n(&g_ov.state, __ATOMIC_RELAXED) : OV_CLOSED;
}

int vm_overlay_cursor(void)
{
	return __atomic_load_n(&g_ov.cursor, __ATOMIC_RELAXED);
}

void vm_overlay_set_mask(uint32_t mask)
{
	__atomic_store_n(&g_mask, mask, __ATOMIC_RELAXED);
}

uint32_t vm_overlay_take_op(void)
{
	uint32_t op;

	while (__sync_lock_test_and_set(&g_lock, 1))
		ksceKernelDelayThread(100);
	op = g_ov.op;
	g_ov.op = 0;
	__sync_lock_release(&g_lock);
	return op;
}
