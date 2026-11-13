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
 * cache it composes one row at a time in g_row.
 *
 * A frame never waits for the worker: `g_seq` is odd while the worker
 * changes the cache, and a frame that saw it odd, or changed while it
 * copied, draws the panel again straight from the text (it overwrites the
 * whole panel). `g_readers` counts the frames inside, so a block is freed
 * only once none can still be reading it. A row for each index (0, 1): the
 * frames of the two layers may be drawn at once. */
static uint32_t     g_row[2][HUD_ROW_MAX];
static volatile int g_row_busy[2];
static volatile uint32_t g_seq;
static volatile int g_readers;
static HudCache     g_cache;
static SceUID       g_cache_uid = -1;
static SceUID       g_cache_stale = -1;   /* freed at the next swap */
static uint32_t     g_gen;

/* ---- the set-framebuffer hook ------------------------------------------- */

/* pParam->base is a user-space CDRAM VA. Writing it from this kernel hook
 * faults under the default ARM domain permissions, so open the Domain Access
 * Control Register for the duration of the blit, then restore it - the
 * standard Vita kernel-overlay technique. */
#ifdef __arm__
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
#else                                  /* the host tests: nothing to open */
static inline uint32_t hud_dacr_open(void) { return 0; }
static inline void hud_dacr_restore(uint32_t prev) { (void)prev; }
#endif

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

/* `t` onto the frame: copied from the cache if it holds `t` and the worker
 * did not touch it meanwhile, else composed straight from the text. */
static void draw_panel(uint32_t *fb, int pitch, int w, int h, const HudText *t, uint32_t *row)
{
	const uint32_t s1 = __atomic_load_n(&g_seq, __ATOMIC_ACQUIRE);
	HudCache c = g_cache;
	int ok = !(s1 & 1) && hud_cache_blit(fb, pitch, w, h, t, &c, row);

	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if (!ok || __atomic_load_n(&g_seq, __ATOMIC_RELAXED) != s1)
		hud_draw_panel(fb, pitch, w, h, t, row);
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
	if (g_text[g_front].n <= 0 && g_ov[g_ov_front].n <= 0)
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

	/* the row: only another frame of the same layer at once could hold it
	 * (the worker never does) */
	{
		volatile int *busy = &g_row_busy[index & 1];
		int got = 0;

		for (int i = 0; i < 4096 && !(got = !__sync_lock_test_and_set(busy, 1)); i++)
			__asm__ volatile("" ::: "memory");
		if (!got)
			return;
		__atomic_fetch_add(&g_readers, 1, __ATOMIC_SEQ_CST);
		/* read once counted: the worker writes a back copy only when no
		 * frame is inside, so the copy read here is not written under it */
		t = &g_text[g_front];
		ov = &g_ov[g_ov_front];

		uint32_t prev_dacr = hud_dacr_open();
		uint32_t *row = g_row[index & 1];

		/* the HUD: from the cache the worker composed (vm_hud_set_text) */
		if (t->n > 0)
			draw_panel((uint32_t *)pParam->base, pitch, w, h, t, row);
		if (ov->n > 0)
			draw_panel((uint32_t *)pParam->base, pitch, w, h, ov, row);
		hud_dacr_restore(prev_dacr);
		__atomic_fetch_sub(&g_readers, 1, __ATOMIC_SEQ_CST);
		__sync_lock_release(busy);
	}
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

/* The worker changing the cache: the frames meanwhile draw from the text. */
static void cache_begin(void)
{
	__atomic_add_fetch(&g_seq, 1, __ATOMIC_SEQ_CST);     /* odd */
}
static void cache_end(void)
{
	__atomic_add_fetch(&g_seq, 1, __ATOMIC_SEQ_CST);     /* even */
}

/* No frame inside the drawing (one is a few us); 0 if one is still there
 * after 100 ms. */
static int readers_gone(void)
{
	for (int i = 0; i < 1000; i++) {
		if (!__atomic_load_n(&g_readers, __ATOMIC_SEQ_CST))
			return 1;
		ksceKernelDelayThread(100);
	}
	return 0;
}

/* Put `uid`/`px`/`bytes` in as the cache (uid -1: no cache) and free the old
 * one once no frame can be reading it. Worker thread only. */
static void cache_swap(SceUID uid, uint8_t *px, int bytes)
{
	SceUID old;

	cache_begin();
	old = g_cache_uid;
	g_cache_uid = uid;
	g_cache.px = px;
	g_cache.bytes = bytes;
	g_cache.w = 0;                     /* nothing composed in it yet */
	cache_end();
	if (readers_gone()) {
		if (g_cache_stale >= 0)
			ksceKernelFreeMemBlock(g_cache_stale);
		g_cache_stale = -1;
		if (old >= 0)
			ksceKernelFreeMemBlock(old);
	} else if (old >= 0) {
		/* a frame held up: the old block kept until the next swap; one
		 * kept from before is long unread by then */
		if (g_cache_stale >= 0)
			ksceKernelFreeMemBlock(g_cache_stale);
		g_cache_stale = old;
	}
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

/* Compose `t` into the cache. The frames meanwhile draw the panel straight
 * from the text: composed in place while a frame copied it, the panel
 * blinked when its text changed. */
static void cache_compose(const HudText *t)
{
	if (!g_cache.px)
		return;
	cache_begin();
	g_cache.w = 0;
	hud_cache_compose(t, g_fb_w, g_fb_h, &g_cache);
	cache_end();
}

void vm_hud_fini(void)
{
	g_text[0].n = g_text[1].n = 0;
	g_ov[0].n = g_ov[1].n = 0;
	if (g_hook >= 0) {
		taiHookReleaseForKernel(g_hook, g_ref);
		g_hook = -1;
	}
	if (g_cache_uid >= 0 || g_cache_stale >= 0)
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
				cache_compose(cur);
			}
			return;
		}
	}
	readers_gone();                    /* the back copy: no frame reads it now */
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
		cache_compose(t);
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
				cache_compose(cur);
			}
			return;
		}
	}
	readers_gone();                    /* the back copy: no frame reads it now */
	panel_fill(t, lines, n, 1);
	if (g_text[g_front].n <= 0) {      /* the HUD hidden: the cache is the overlay's */
		cache_fit(t);
		if (n > 0)
			cache_compose(t);
	}
	__sync_synchronize();   /* the text must be visible before the flip */
	g_ov_front = back;
	if (n <= 0)
		cache_release_if_unused();     /* closed: freed, if the HUD is not shown */
}
