/*
 * hud.c - draw the vita-memio HUD over the live framebuffer.
 *
 * Technique (the standard taiHEN overlay idiom): hook
 * SceDisplay's set-framebuffer funnel. The caller hands us the framebuffer
 * base, pitch and size every frame; we let the original run, then draw our
 * text into the same buffer before the display controller scans it out.
 *
 * Strictly read + overdraw: no storage state is touched here. The text comes
 * from the 1 Hz worker (main.c) through vm_hud_set_text(), double-buffered so
 * the frame hook never reads a half-written line. The drawing itself is in
 * hud_draw.h (top right corner, classic or PSVshell style).
 *
 * Drawing is guarded by a plausibility check on width, height, pitch and pixel
 * format and is simply skipped if the frame looks wrong, rather than risk a bad
 * write. (The framebuffer descriptor carries no buffer length, so the real
 * buffer size cannot be checked; the limits below are what can be.)
 */
#include <psp2kern/kernel/sysclib.h>   /* memcpy/memset                        */
#include <psp2kern/display.h>
#include <taihen.h>

#include "hud.h"
#include "hud_draw.h"
#include "klog.h"
#include "../include/vitamemio.h"

static SceUID         g_hook = -1;
static tai_hook_ref_t g_ref;

/* Double-buffered: the worker fills the back copy, then flips g_front. */
static HudText      g_text[2];
static volatile int g_front = 0;

/* The PSVshell style composes one row at a time here (ordinary cached memory)
 * and copies it out; `g_row_busy` keeps two drawing threads apart. */
static uint32_t     g_row[HUD_ROW_MAX];
static volatile int g_row_busy;

/* ---- the set-framebuffer hook ------------------------------------------- */

/* pParam->base is a user-space CDRAM VA. Writing it from this kernel hook
 * faults under the default ARM domain permissions, so open the Domain Access
 * Control Register for the duration of the blit, then restore it - the
 * standard Vita kernel-overlay technique. */
static inline uint32_t hud_dacr_open(void)
{
	uint32_t prev;
	__asm__ volatile("mrc p15, 0, %0, c3, c0, 0" : "=r"(prev));
	__asm__ volatile("mcr p15, 0, %0, c3, c0, 0" :: "r"(0xFFFFFFFFu));
	__asm__ volatile("isb");
	return prev;
}
static inline void hud_dacr_restore(uint32_t prev)
{
	__asm__ volatile("mcr p15, 0, %0, c3, c0, 0" :: "r"(prev));
	__asm__ volatile("isb");
}

/* ksceDisplaySetFrameBufInternal(head, index, pParam, sync) - the kernel
 * funnel every set-framebuffer call passes through, so the overlay is
 * system-wide. Draw only for the main panel (head 0). */
static int hook_set_framebuf(int head, int index,
                             const SceDisplayFrameBuf *pParam, int sync)
{
	int ret = TAI_CONTINUE(int, g_ref, head, index, pParam, sync);
	const HudText *t;

	if (head != 0 || !pParam || !pParam->base)
		return ret;
	t = &g_text[g_front];
	if (t->n <= 0)
		return ret;
	int w = (int)pParam->width, h = (int)pParam->height,
	    pitch = (int)pParam->pitch;

	/* Plausibility guard: only draw into a sane 32bpp linear buffer. */
	if (w <= 0 || h <= 0 || pitch < w || pitch > 2048 || w > 1920 || h > 1088)
		return ret;
	if (pParam->pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8)
		return ret;

	uint32_t prev_dacr = hud_dacr_open();
	if (t->style == VM_HUD_STYLE_PSVSHELL) {
		if (!__sync_lock_test_and_set(&g_row_busy, 1)) {
			hud_draw_panel((uint32_t *)pParam->base, pitch, w, h, t, g_row);
			__sync_lock_release(&g_row_busy);
		}
	} else {
		hud_draw_classic((uint32_t *)pParam->base, pitch, w, h, t);
	}
	hud_dacr_restore(prev_dacr);
	return ret;
}

int vm_hud_init(void)
{
	memset(g_text, 0, sizeof(g_text));

	/* NIDs from the VitaSDK db (vita-headers db/360/SceDisplay.yml), present
	 * unchanged in 3.65 and 3.68:
	 *   SceDisplayForDriver library    = 0x9FED47AC
	 *   ksceDisplaySetFrameBufInternal = 0x16466675 */
	g_hook = taiHookFunctionExportForKernel(KERNEL_PID, &g_ref,
	                                        "SceDisplay",
	                                        0x9FED47AC, /* SceDisplayForDriver  */
	                                        0x16466675, /* ...SetFrameBufInternal */
	                                        hook_set_framebuf);
	if (g_hook < 0) {
		VM_KLOGW("hud: set-framebuffer hook failed (0x%08x)", (unsigned)g_hook);
		return VM_ERR_RESOLVE;
	}
	return VM_OK;
}

void vm_hud_fini(void)
{
	g_text[0].n = g_text[1].n = 0;
	if (g_hook >= 0) {
		taiHookReleaseForKernel(g_hook, g_ref);
		g_hook = -1;
	}
}

int vm_hud_installed(void)
{
	return g_hook >= 0;
}

void vm_hud_set_text(const char lines[][VM_HUD_LINE_MAX], int n, int style)
{
	int back = g_front ^ 1;
	HudText *t = &g_text[back];
	if (n < 0) n = 0;
	if (n > VM_HUD_LINES) n = VM_HUD_LINES;
	t->maxlen = 0;
	for (int i = 0; i < n; i++) {
		int len = 0;
		while (len < VM_HUD_LINE_MAX - 1 && lines[i][len]) {
			t->lines[i][len] = lines[i][len];
			len++;
		}
		t->lines[i][len] = 0;
		len = hud_visible_len(t->lines[i]);
		if (len > t->maxlen)
			t->maxlen = len;
	}
	t->style = style;
	t->n = n;
	__sync_synchronize();   /* the text must be visible before the flip */
	g_front = back;
}
