/*
 * overlay.c - the SELECT + arrows overlay's keys (see overlay_logic.h).
 *
 * The buttons are seen where every app and game reads them: the SceCtrl
 * syscalls (sceCtrlPeek/ReadBufferPositive/Negative, ...2, ...Ext, ...Ext2),
 * hooked in the kernel. After the original fills the caller's buffers, the
 * newest one is looked at; while the overlay is open the buttons in all of
 * them are cleared (sticks left alone), so the game does not act on them.
 * Nothing else is changed, and with the setting off the hooks are not there.
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
static tai_hook_ref_t g_ref[OV_FNS];
static int            g_installed;

static OvLogic        g_ov;
static volatile int   g_lock;
static void (*volatile g_wake)(void);

/* The buttons of the newest buffer seen, through the logic; then, if the
 * overlay is open, cleared in all `count` of them. */
static void on_pad(SceCtrlData *pad, int count, int negative)
{
	uint32_t b = 0;
	int changed, hide;

	if (!pad || count <= 0 || count > 64)
		return;
	if (ksceKernelMemcpyUserToKernel(&b, (const char *)&pad[count - 1] +
	                                 offsetof(SceCtrlData, buttons), sizeof(b)) < 0)
		return;
	if (negative)
		b = ~b;
	if (__sync_lock_test_and_set(&g_lock, 1)) {
		/* another thread is at it: its look counts; the hiding still applies */
		changed = 0;
		hide = __atomic_load_n(&g_ov.state, __ATOMIC_RELAXED) == OV_MENU;
	} else {
		changed = ov_input(&g_ov, b);
		hide = g_ov.state == OV_MENU;
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
	}
}

#define OV_HOOK(i)                                                               \
	static int ov_fn##i(int port, SceCtrlData *pad, int count)                   \
	{                                                                            \
		int r = TAI_CONTINUE(int, g_ref[i], port, pad, count);                   \
		if (r > 0)                                                               \
			on_pad(pad, r < count ? r : count, k_fn[i].negative);                \
		return r;                                                                \
	}
OV_HOOK(0) OV_HOOK(1) OV_HOOK(2) OV_HOOK(3) OV_HOOK(4)
OV_HOOK(5) OV_HOOK(6) OV_HOOK(7) OV_HOOK(8) OV_HOOK(9)

static const void *const k_hooks[OV_FNS] = {
	ov_fn0, ov_fn1, ov_fn2, ov_fn3, ov_fn4, ov_fn5, ov_fn6, ov_fn7, ov_fn8, ov_fn9,
};

void vm_overlay_enable(int on, void (*wake)(void))
{
	__atomic_store_n(&g_wake, wake, __ATOMIC_RELEASE);
	if (on && !g_installed) {
		int ok = 0;

		/* afresh: nothing of an earlier time (a hook still running when they
		 * were released may have left a state) */
		while (__sync_lock_test_and_set(&g_lock, 1))
			ksceKernelDelayThread(100);
		g_ov.state = OV_CLOSED;
		g_ov.op = 0;
		g_ov.prev = 0;
		g_ov.cursor = 0;
		__sync_lock_release(&g_lock);

		for (int i = 0; i < OV_FNS; i++) {
			g_hook[i] = taiHookFunctionExportForKernel(KERNEL_PID, &g_ref[i], "SceCtrl",
			                                           TAI_ANY_LIBRARY, k_fn[i].nid, k_hooks[i]);
			ok += g_hook[i] >= 0;
		}
		g_installed = 1;
		if (ok == OV_FNS)
			VM_KLOGI("overlay: ready (SELECT + arrows)");
		else
			VM_KLOGW("overlay: %d of %d button hooks installed", ok, OV_FNS);
	} else if (!on && g_installed) {
		for (int i = 0; i < OV_FNS; i++)
			if (g_hook[i] >= 0) {
				taiHookReleaseForKernel(g_hook[i], g_ref[i]);
				g_hook[i] = -1;
			}
		g_installed = 0;
		while (__sync_lock_test_and_set(&g_lock, 1))
			ksceKernelDelayThread(100);
		g_ov.state = OV_CLOSED;
		g_ov.op = 0;
		__sync_lock_release(&g_lock);
	}
}

int vm_overlay_installed(void)
{
	return g_installed;
}

int vm_overlay_state(void)
{
	return g_installed ? __atomic_load_n(&g_ov.state, __ATOMIC_RELAXED) : OV_CLOSED;
}

int vm_overlay_cursor(void)
{
	return __atomic_load_n(&g_ov.cursor, __ATOMIC_RELAXED);
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
