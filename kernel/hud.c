/*
 * hud.c - draw the vita-memio HUD over the live framebuffer.
 *
 * Technique (the standard taiHEN overlay idiom): hook
 * SceDisplay's set-framebuffer funnel. The caller hands us the framebuffer
 * base, pitch and size every frame; we draw our text into that buffer, then
 * let the original hand it to the display (drawn after, it blinked).
 *
 * Strictly read + overdraw: no storage state is touched here. The text comes
 * from the worker (hud_main.c, every second while the HUD is on) through vm_hud_set_text(), double-buffered so
 * the frame hook never reads a half-written line. The drawing itself is in
 * hud_draw.h.
 *
 * Drawing is guarded by a plausibility check on width, height, pitch and pixel
 * format and is simply skipped if the frame looks wrong, rather than risk a bad
 * write. (The framebuffer descriptor carries no buffer length, so the real
 * buffer size cannot be checked; the limits below are what can be.)
 */
#include <psp2kern/kernel/sysclib.h>   /* memcpy/memset/memcmp                 */
#include <psp2kern/kernel/sysmem.h>    /* the panel cache's memory block       */
#include <psp2kern/kernel/threadmgr.h> /* ksceKernelDelayThread                */
#include <psp2kern/display.h>
#include <taihen.h>

#include "hud.h"
#include "hud_draw.h"
#include "klog.h"
#include "vm_hook.h"
#include "../include/vitamemio.h"

int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid,
                           uint32_t funcnid, uintptr_t *func);   /* taiHEN */

static SceUID         g_hook = -1;
static volatile tai_hook_ref_t g_ref;

/* Double-buffered: the worker fills the back copy, then flips g_front. */
static HudText      g_text[2];
static volatile int g_front = 0;
/* The overlay's, the same way; drawn without the cache (it is open briefly). */
static HudText      g_ov[2];
static volatile int g_ov_front = 0;

/* The HUD keeps the composed panel in g_cache, at four bits a pixel
 * (ordinary cached memory, allocated by the worker while the HUD is on), so a frame only copies
 * it; built again when the text or the framebuffer size changes. Without the
 * cache it composes one row at a time in g_row. `g_row_busy` keeps the drawing
 * threads and the worker's cache swap apart. */
static uint32_t     g_row[HUD_ROW_MAX];
static volatile int g_row_busy;
static HudCache     g_cache;
static SceUID       g_cache_uid = -1;
static uint32_t     g_gen;

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

/* Which (head, index) pairs the hook has seen, a bit each (head 0..1 x
 * index 0..1, then any other), for the log: vm_hud_planes. */
static volatile uint32_t g_planes_seen;
/* The width of the frames drawn on, for the cache's size (its font); until a
 * frame is seen, the largest font's. */
static volatile int g_fb_w = 960, g_fb_h = 544;
static int g_text_width;               /* the HUD's widest line while shown */
/* ksceAppMgrIsExclusiveProcessRunning, looked up at vm_hud_init (not every
 * SDK's stubs have it); NULL: not there - index 1 is then never drawn on. */
static int (*g_excl_running)(const char *title_id);

uint32_t vm_hud_planes(void)
{
	return __atomic_load_n(&g_planes_seen, __ATOMIC_RELAXED);
}

/* The HUD and the overlay drawn into the frame, as PSVshell draws (which
 * does not blink): on the head that is not 0, and on index 1 (the system's
 * own layer over a game: its menus and notices) only while no game runs.
 * Drawn on head 0 and on that layer as well, it blinked at times and showed
 * where it should not. */
static void draw_frame(int head, int index, const SceDisplayFrameBuf *pParam)
{
	const HudText *t, *ov;
	const uint32_t bit = head >= 0 && head < 2 && index >= 0 && index < 2
	                     ? 1u << (head * 2 + index) : 1u << 4;

	if (!(__atomic_load_n(&g_planes_seen, __ATOMIC_RELAXED) & bit))
		__atomic_fetch_or(&g_planes_seen, bit, __ATOMIC_RELAXED);
	if (!head || !pParam || !pParam->base)
		return;
	if (index && (!g_excl_running || g_excl_running(NULL) > 0))
		return;
	t = &g_text[g_front];
	ov = &g_ov[g_ov_front];
	if (t->n <= 0 && ov->n <= 0)
		return;
	int w = (int)pParam->width, h = (int)pParam->height,
	    pitch = (int)pParam->pitch;

	/* Plausibility guard: only draw into a sane 32bpp linear buffer. */
	if (w <= 0 || h <= 0 || pitch < w || pitch > 2048 || w > 1920 || h > 1088)
		return;
	if (pParam->pixelformat != SCE_DISPLAY_PIXELFORMAT_A8B8G8R8)
		return;
	if (g_fb_w != w || g_fb_h != h) {
		g_fb_w = w;
		g_fb_h = h;
	}

	/* the lock is held only for a few instructions elsewhere (the cache
	 * swapped): wait for it rather than leave a frame without the panel */
	{
		int got = 0;

		for (int i = 0; i < 4096 && !(got = !__sync_lock_test_and_set(&g_row_busy, 1)); i++)
			__asm__ volatile("" ::: "memory");
		if (!got)
			return;                      /* not had: not drawn this once */
	}
	uint32_t prev_dacr = hud_dacr_open();
	/* the HUD: from the cache the worker composed (vm_hud_set_text); only if
	 * it is not there for this text and size, composed here */
	if (t->n > 0 &&
	    !hud_cache_blit((uint32_t *)pParam->base, pitch, w, h, t, &g_cache, g_row))
		hud_draw_panel((uint32_t *)pParam->base, pitch, w, h, t, g_row);
	if (ov->n > 0 &&
	    !hud_cache_blit((uint32_t *)pParam->base, pitch, w, h, ov, &g_cache, g_row))
		hud_draw_panel((uint32_t *)pParam->base, pitch, w, h, ov, g_row);
	hud_dacr_restore(prev_dacr);
	__sync_lock_release(&g_row_busy);
}

/* ksceDisplaySetFrameBufInternal(head, index, pParam, sync) - the kernel
 * funnel every set-framebuffer call passes through, so the overlay is
 * system-wide. Drawn before the frame is handed on: once the original has it, the display
 * may already be showing it, and a frame whose panel took longer (the text
 * just changed, so the panel is composed again) blinked without it. */
static int hook_set_framebuf(int head, int index,
                             const SceDisplayFrameBuf *pParam, int sync)
{
	const tai_hook_ref_t ref = vm_hook_ref(&g_ref);

	if (!ref)
		return VM_HOOK_NO_REF;
	draw_frame(head, index, pParam);
	return TAI_CONTINUE(int, ref, head, index, pParam, sync);
}

int vm_hud_init(void)
{
	memset(g_text, 0, sizeof(g_text));
	memset(g_ov, 0, sizeof(g_ov));

	/* NIDs from the VitaSDK db (vita-headers db/360/SceDisplay.yml), present
	 * unchanged in 3.65 and 3.68:
	 *   SceDisplayForDriver library    = 0x9FED47AC
	 *   ksceDisplaySetFrameBufInternal = 0x16466675 */
	{
		uintptr_t f = 0;

		/* SceAppMgrForDriver 0xDCE180F8, ksceAppMgrIsExclusiveProcessRunning
		 * 0xA888C2DC (vita-headers db/360/SceAppMgr.yml) */
		g_excl_running = module_get_export_func(KERNEL_PID, "SceAppMgr", 0xDCE180F8, 0xA888C2DC,
		                                        &f) >= 0 && f
		                 ? (int (*)(const char *))f : NULL;
		if (!g_excl_running)
			VM_KLOGW("hud: no ksceAppMgrIsExclusiveProcessRunning: the system's layer is left alone");
	}
	g_ref = 0;                             /* not the last time's: see vm_hook_ref */
	g_hook = taiHookFunctionExportForKernel(KERNEL_PID, (tai_hook_ref_t *)&g_ref,
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

/* Put `uid`/`px`/`words` in as the cache (uid -1: no cache) and free the old
 * one, with the drawing threads kept out meanwhile. Worker thread only. */
static void cache_swap(SceUID uid, uint8_t *px, int bytes)
{
	SceUID old;
	int got = 0;

	/* a frame being drawn: a few us. If its thread is held up longer, the
	 * old block stays (its memory too) rather than being freed under it. */
	for (int i = 0; i < 1000 && !(got = !__sync_lock_test_and_set(&g_row_busy, 1)); i++)
		ksceKernelDelayThread(100);
	if (!got) {
		if (uid >= 0)
			ksceKernelFreeMemBlock(uid);   /* the new one is not needed then */
		return;
	}
	old = g_cache_uid;
	g_cache_uid = uid;
	g_cache.px = px;
	g_cache.bytes = bytes;
	g_cache.w = 0;                     /* nothing composed in it yet */
	__sync_lock_release(&g_row_busy);
	if (old >= 0)
		ksceKernelFreeMemBlock(old);
}

/* A cache big enough for `t` (at the largest font), or none if the HUD is off
 * If the memory is not there, the panel is just
 * composed every frame. */
static void cache_release_if_unused(void);

static void cache_fit(const HudText *t)
{
	static int failed_bytes;           /* the size that could not be had */
	SceKernelAllocMemBlockKernelOpt opt;
	SceUID uid;
	void *base = NULL;
	int want, size;

	if (t->n <= 0) {
		failed_bytes = 0;
		return;
	}
	want = hud_cache_bytes(t, g_fb_w);
	if (want <= g_cache.bytes || want == failed_bytes)
		return;
	size = (want + 0xFFF) & ~0xFFF;
	memset(&opt, 0, sizeof(opt));
	opt.size = sizeof(opt);
	uid = ksceKernelAllocMemBlock("SceVitaMemioHud", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_RW,
	                              size, &opt);
	if (uid < 0 || ksceKernelGetMemBlockBase(uid, &base) < 0) {
		if (uid >= 0)
			ksceKernelFreeMemBlock(uid);
		failed_bytes = want;
		return;
	}
	cache_swap(uid, base, size);
}

void vm_hud_fini(void)
{
	g_text[0].n = g_text[1].n = 0;
	g_ov[0].n = g_ov[1].n = 0;
	if (g_hook >= 0) {
		taiHookReleaseForKernel(g_hook, g_ref);
		g_hook = -1;
	}
	if (g_cache_uid >= 0)
		cache_swap(-1, NULL, 0);
}

int vm_hud_installed(void)
{
	return g_hook >= 0;
}

void vm_hud_set_text(const char lines[][VM_HUD_LINE_MAX], int n)
{
	int back = g_front ^ 1;
	HudText *t = &g_text[back];
	const HudText *cur = &g_text[g_front];
	if (n < 0) n = 0;
	if (n > VM_HUD_LINES) n = VM_HUD_LINES;
	/* The same text as on screen: nothing to do (the cache stays valid),
	 * unless the frames are another size now. */
	if (n == cur->n) {
		int same = 1;
		for (int i = 0; i < n && same; i++)
			same = !strncmp(lines[i], cur->lines[i], VM_HUD_LINE_MAX);
		if (same) {
			if (n > 0 && (g_cache.gen != cur->gen || g_cache.w != g_fb_w || g_cache.h != g_fb_h)) {
				cache_fit(cur);
				hud_cache_compose(cur, g_fb_w, g_fb_h, &g_cache);
			}
			return;
		}
	}
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
	/* as wide as it has been while shown: a number one character longer no
	 * longer moves the panel's edge (PSVshell's panel has a fixed frame) */
	if (n > 0 && t->maxlen < g_text_width)
		t->maxlen = g_text_width;
	g_text_width = n > 0 ? t->maxlen : 0;
	t->n = n;
	t->gen = ++g_gen;
	cache_fit(t);
	/* composed here, on the worker: the frame hook only expands it */
	if (n > 0)
		hud_cache_compose(t, g_fb_w, g_fb_h, &g_cache);
	__sync_synchronize();   /* the text must be visible before the flip */
	g_front = back;
	if (n <= 0)
		cache_release_if_unused();     /* hidden: freed, if the overlay is not shown */
}

/* Fill `t` (the back copy) as a top-left panel. */
static void panel_fill(HudText *t, const char lines[][VM_HUD_LINE_MAX], int n, int where)
{
	static int width;                  /* as wide as it has been while shown */

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
	if (n > 0 && t->maxlen < width)
		t->maxlen = width;
	width = n > 0 ? t->maxlen : 0;
	t->left = where;
	t->n = n;
	t->gen = ++g_gen;
}

/* The cache freed once neither the HUD nor the overlay is shown (they share
 * it: never both at once). After a flip. */
static void cache_release_if_unused(void)
{
	if (g_cache_uid >= 0 && g_text[g_front].n <= 0 && g_ov[g_ov_front].n <= 0)
		cache_swap(-1, NULL, 0);
}

/* The overlay: composed here into the cache the HUD uses (they are never
 * shown at once: the HUD's text is empty while the menu is open), so the
 * frame hook only expands it, as for the HUD; the same text again (it is
 * set four times a second) is not composed again. */
void vm_hud_set_overlay(const char lines[][VM_HUD_LINE_MAX], int n)
{
	int back = g_ov_front ^ 1;
	HudText *t = &g_ov[back];
	const HudText *cur = &g_ov[g_ov_front];

	if (n > VM_HUD_LINES) n = VM_HUD_LINES;
	if (n == cur->n && n > 0) {
		int same = 1;

		for (int i = 0; i < n && same; i++)
			same = !strncmp(lines[i], cur->lines[i], VM_HUD_LINE_MAX);
		if (same) {
			if (g_text[g_front].n <= 0 &&
			    (g_cache.gen != cur->gen || g_cache.w != g_fb_w || g_cache.h != g_fb_h)) {
				cache_fit(cur);
				hud_cache_compose(cur, g_fb_w, g_fb_h, &g_cache);
			}
			return;
		}
	}
	panel_fill(t, lines, n, 1);
	if (g_text[g_front].n <= 0) {      /* the HUD hidden: the cache is the overlay's */
		cache_fit(t);
		if (n > 0)
			hud_cache_compose(t, g_fb_w, g_fb_h, &g_cache);
	}
	__sync_synchronize();   /* the text must be visible before the flip */
	g_ov_front = back;
	if (n <= 0)
		cache_release_if_unused();     /* closed: freed, if the HUD is not shown */
}
