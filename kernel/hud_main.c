/*
 * hud_main.c - vitamemio_hud.skprx, the HUD module: the HUD, the SELECT +
 * arrows overlay and its read ahead test menu.
 *
 * Not in config.txt: the kernel module (vitamemio_kernel.skprx) loads it at
 * the start when the overlay is on, and calls it from its worker
 * (kernel/hud_link.h). Each call writes the HUD's and the overlay's text from
 * the kernel module's state and settings; the drawing is in hud.c, the keys in
 * overlay.c. Everything it reads or changes is the kernel module's, through
 * the functions it was handed (g_core), so this module holds no state of the
 * plugin's: only its own hooks, text and the panel cache.
 */
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>
#include <stdarg.h>

#include "../include/vitamemio.h"
#include "hud.h"
#include "hud_link.h"
#include "klog.h"
#include "overlay.h"
#include "overlay_logic.h"
#include "settings.h"
#include "stage.h"

static const VmCoreApi *g_core;

/* ---- the kernel module's functions (as stage.h, settings.h and klog.h
 * declare them), through g_core ------------------------------------------------ */

void vm_vklog(int level, const char *fmt, va_list ap)
{
	g_core->vlog(level, fmt, ap);
}

void vm_klog(int level, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	g_core->vlog(level, fmt, ap);
	va_end(ap);
}

void vm_settings_copy(VmSettings *out)          { g_core->settings_copy(out); }
void vm_stage_bytes(int dev, uint64_t *rd, uint64_t *wr) { g_core->bytes(dev, rd, wr); }
int  vm_stage_memcard_slow(void)                { return g_core->memcard_slow(); }
void vm_stage_fg(VmFg *out)                     { g_core->fg(out); }
void vm_stage_ra_now(VmRaNow *out)              { g_core->ra_now(out); }
void vm_stage_core_stats(uint32_t cp_core_kib[4], uint32_t *nc_sys_kib, uint32_t *nc_own_kib)
{
	g_core->core_stats(cp_core_kib, nc_sys_kib, nc_own_kib);
}
void vm_stage_count_cores(int on)               { g_core->count_cores(on); }
void vm_stage_count_hud(int on)                 { g_core->count_hud(on); }
void vm_stage_app_now(VmAppNow *out)            { g_core->app_now(out); }
int  vm_stage_app_set(const VmApp *a)           { return g_core->app_set(a); }
int  vm_stage_app_save(void)                    { return g_core->app_save(); }
int  vm_stage_ratest_start(void)                { return g_core->ratest_start(); }
int  vm_stage_ratest_running(uint32_t *mib)     { return g_core->ratest_running(mib); }
int  vm_stage_ratest_stale(void)                { return g_core->ratest_stale(); }
void vm_stage_ratest_cancel(void)               { g_core->ratest_cancel(); }
void vm_stage_ratest_stop(VmRaTestPick *out)    { g_core->ratest_stop(out); }

/* ---- the HUD's and the overlay's text ------------------------------------------ */

/* The storages the plugin speeds up, in HUD order (never the internal one). */
static const int k_hud_order[] = { VM_DEV_SD2VITA, VM_DEV_MEMCARD };
#define HUD_DEVS ((int)(sizeof(k_hud_order) / sizeof(k_hud_order[0])))

/* `bytes` moved in `ms` milliseconds, as MiB/s with one decimal. The worker
 * does not always wake exactly a second apart (a changed setting wakes it
 * early, and it sleeps longer while the HUD is off), so the time is measured. */
static void mbps(char *o, int n, uint64_t bytes, uint32_t ms)
{
	uint32_t t = ms ? (uint32_t)(((bytes * 10000u) / ms) >> 20) : 0;   /* tenths */
	snprintf(o, n, "%u.%u", (unsigned)(t / 10), (unsigned)(t % 10));
}

/* `s` cut to `max` characters and padded to `width`, on the left (right = 1)
 * or the right. The system snprintf is only relied on for plain %s/%u. */
static void __attribute__((noinline)) pad(char *o, int cap, const char *s, int width, int max,
                                           int right)
{
	int len = 0, n = 0;

	while (len < max && s[len])
		len++;
	if (width > cap - 1)
		width = cap - 1;
	if (len > cap - 1)
		len = cap - 1;
	if (right)
		while (n < width - len)
			o[n++] = ' ';
	for (int i = 0; i < len; i++)
		o[n++] = s[i];
	while (n < width)
		o[n++] = ' ';
	o[n] = 0;
}

/* The colour of a storage's values: what its faster path is doing. */
static const char *state_color(const VmDevState *d, int slow)
{
	switch (d->status) {
	case VM_ST_ACTIVE:   return slow ? HUD_C_WARN : HUD_C_GOOD;
	case VM_ST_FALLBACK: return HUD_C_BAD;
	case VM_ST_OFF:      return HUD_C_TEXT;
	default:             return HUD_C_WARN;
	}
}

/* The HUD is the overlay's: it shows only by the overlay's keys (SELECT +
 * Right from the overlay), always the extended one; with the overlay off
 * there is none. */
static int hud_shown(const VmSettings *s)
{
	return s->overlay && vm_overlay_installed() && vm_overlay_state() == OV_HUD &&
	       vm_hud_installed();
}

/* Where copies run is counted (vm_stage_count_cores) while the overlay is
 * open or the HUD shows Auto's line; each says what it needs here. */
static int g_cnt_hud, g_cnt_ov;

static void count_cores_want(int *who, int on)
{
	*who = on;
	vm_stage_count_cores(g_cnt_hud || g_cnt_ov);
}

/* "100%" coloured as the overlay does, or "-" without data. */
static void pct_text(char *o, int cap, int p)
{
	if (p < 0)
		snprintf(o, cap, HUD_C_DIM "-");
	else
		snprintf(o, cap, "%s%u%%", p >= 90 ? HUD_C_GOOD : p > 0 ? HUD_C_WARN : HUD_C_BAD,
		         (unsigned)(p > 100 ? 100 : p));
}

/* A threshold for the HUD: the number, or "never" (never starts); with
 * Adaptive "set:now", e.g. "5:6". */
static void hud_thr(char *o, int cap, uint32_t set, uint32_t now, int adaptive)
{
	if (now == VM_RA_NEVER)
		snprintf(o, cap, HUD_C_DIM "never");
	else if (adaptive)
		snprintf(o, cap, HUD_C_TEXT "%u:" HUD_C_GOOD "%u", (unsigned)(set > 99 ? 99 : set),
		         (unsigned)(now > 99 ? 99 : now));
	else
		snprintf(o, cap, HUD_C_GOOD "%u", (unsigned)(now > 99 ? 99 : now));
}

/* "B": the app's buffers for the system core (VmRaNow.bufmap) - green all
 * mapped, yellow some from their 2nd page (worked around), red some refused;
 * grey while none is mapped (or the setting is off): always there, so the
 * line does not shift. */
static const char *hud_bufmap(int b)
{
	return b == VM_BUFMAP_OK ? HUD_C_GOOD "B" : b == VM_BUFMAP_WORKAROUND ? HUD_C_WARN "B"
	     : b == VM_BUFMAP_REFUSED ? HUD_C_BAD "B" : HUD_C_DIM "B";
}

/* Read ahead in the HUD, for the app in front:
 *   R/A: On  TitleID: PCSB00777          (On, Adaptive or Off)
 *   Streak  C: 5  NC: never              (reads in a row before it starts;
 *                                         Adaptive: as set:now, C: 5:6)
 *   Core 3 B C: 98%  NC: 100%            (since the HUD came up; B: hud_bufmap)
 * Off: no Streak line. Returns the lines added (at most `room`). */
static int hud_ra_lines(char (*L)[VM_HUD_LINE_MAX], int room, const VmFg *fg, int on)
{
	static int counting;
	static uint32_t base_core[4], base_ncs, base_nco;
	char c[16], nc[16];
	const char *mode;
	VmRaNow now;
	int n = 0;

	/* where copies run: counted only while the HUD shows it */
	if (on && !counting) {
		count_cores_want(&g_cnt_hud, 1);
		vm_stage_core_stats(base_core, &base_ncs, &base_nco);
		counting = 1;
	} else if (!on && counting) {
		count_cores_want(&g_cnt_hud, 0);
		counting = 0;
	}
	if (!on || room <= 0)
		return 0;
	vm_stage_ra_now(&now);
	/* in effect for the app in front (its own, or the global one) */
	mode = now.mode == VM_RA_ON ? HUD_C_TEXT "On" :
	       now.mode == VM_RA_ADAPTIVE ? HUD_C_TEXT "Adaptive" : HUD_C_DIM "Off";
	hud_thr(c, sizeof(c), now.set[0], now.thr[0], now.mode == VM_RA_ADAPTIVE);
	hud_thr(nc, sizeof(nc), now.set[1], now.thr[1], now.mode == VM_RA_ADAPTIVE);
	snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "R/A: %s  " HUD_C_DIM "TitleID: " HUD_C_TEXT
	         "%.15s", mode, fg->title);
	if (now.mode != VM_RA_OFF && n < room)
		snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "Streak  C: %.9s" HUD_C_DIM
		         "  NC: %.9s", c, nc);
	if (n < room) {
		uint32_t core[4], ncs, nco, other = 0;
		char a[16], b[16];

		vm_stage_core_stats(core, &ncs, &nco);
		for (int i = 0; i < 3; i++)
			other += core[i] - base_core[i];
		pct_text(a, sizeof(a), ov_pct(core[3] - base_core[3], other));
		pct_text(b, sizeof(b), ov_pct(ncs - base_ncs, nco - base_nco));
		snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "Core 3 %s " HUD_C_DIM "C: %.7s" HUD_C_DIM
		         "  NC: %.7s", hud_bufmap(now.bufmap), a, b);
	}
	return n;
}

static int rt_line(char *o);

static void hud_feed(const VmDevState *now)
{
	static uint64_t last_rd[VM_DEV_COUNT], last_wr[VM_DEV_COUNT], last_us;
	static char L[VM_HUD_LINES][VM_HUD_LINE_MAX];
	static int hud_failed;           /* tried once and failed: do not retry */
	static int was_shown;            /* on screen at the last call         */
	const uint64_t now_us = (uint64_t)ksceKernelGetSystemTimeWide();
	const uint32_t ms = last_us ? (uint32_t)((now_us - last_us) / 1000u) : 1000u;
	VmSettings s;
	int n = 0, shown, fresh;

	last_us = now_us;

	vm_settings_copy(&s);
	/* the overlay draws through the HUD's hook too */
	if (s.overlay && !vm_hud_installed() && !hud_failed &&
	    vm_hud_init() != VM_OK) {
		hud_failed = 1;
		VM_KLOGW("hud: could not install - HUD unavailable");
	}
	{                                /* each new (head, index) the frames came on, once */
		static uint32_t logged;
		const uint32_t seen = vm_hud_planes();

		if (seen & ~logged) {
			logged = seen;
			VM_KLOGI("hud: frames on head/index 0/0 %s, 0/1 %s, 1/0 %s, 1/1 %s, other %s",
			         seen & 1 ? "yes" : "no", seen & 2 ? "yes" : "no", seen & 4 ? "yes" : "no",
			         seen & 8 ? "yes" : "no", seen & 16 ? "yes" : "no");
		}
	}
	shown = hud_shown(&s);
	/* just shown: nothing was counted while it was not, so no speed yet (the
	 * time since the last call is not the time counted) */
	fresh = shown && !was_shown;
	was_shown = shown;
	/* transfers counted while anything shows them */
	vm_stage_count_hud(shown || vm_overlay_state() == OV_MENU);
	/* One line per storage that has seen any I/O since boot (an SD2Vita line
	 * only appears with one fitted): live speed of all its transfers, whether
	 * or not they took the faster path, coloured by what the faster path is
	 * doing. Extended adds totals and state. With an app in front that has
	 * read enough to tell, only the storage it reads from. */
	int slow = vm_stage_memcard_slow() == 1;
	VmFg fg;

	vm_stage_fg(&fg);

	for (int o = 0; o < HUD_DEVS; o++) {
		static uint64_t seen[VM_DEV_COUNT];
		int i = k_hud_order[o];
		uint64_t rd, wr;
		char r[12], w[12], rp[12], wp[12];

		vm_stage_bytes(i, &rd, &wr);
		mbps(r, sizeof(r), fresh ? 0 : rd - last_rd[i], ms);
		mbps(w, sizeof(w), fresh ? 0 : wr - last_wr[i], ms);
		last_rd[i] = rd;
		last_wr[i] = wr;
		seen[i] |= rd | wr;
		/* a storage switched off in Settings is not shown */
		if (!shown || !seen[i] || now[i].status == VM_ST_OFF ||
		    (fg.dev >= 0 && i != fg.dev))
			continue;
		int dev_slow = i == VM_DEV_MEMCARD && slow;
		const char *c = state_color(&now[i], dev_slow);
		/* as wide as the names were padded to (8), not cut */
		const char *np = i == VM_DEV_MEMCARD ? "MEMORY CARD" : "SD2VITA ";
		const char *flag = now[i].status == VM_ST_FALLBACK ? "ERROR"
		                 : dev_slow ? "SLOW" : "";
		pad(rp, sizeof(rp), r, 4, 6, 1);
		pad(wp, sizeof(wp), w, 4, 6, 1);
		snprintf(L[n++], VM_HUD_LINE_MAX,
		         HUD_C_TEXT "%s " HUD_C_DIM "R: %s%s " HUD_C_DIM "W: %s%s " HUD_C_DIM "MB/s%s%s%s",
		         np, c, rp, c, wp, flag[0] ? " " : "", c, flag);
		const char *state =
			now[i].status == VM_ST_ACTIVE   ? "ON" :
			now[i].status == VM_ST_OFF      ? "OFF" :
			now[i].status == VM_ST_FALLBACK ? "STANDARD" :
			                                  "UNAVAILABLE";
		snprintf(L[n++], VM_HUD_LINE_MAX,
		         HUD_C_DIM "R/W: %u/%u MiB %s%s " HUD_C_DIM "%s: %s%u%s%s%s",
		         (unsigned)(rd >> 20), (unsigned)(wr >> 20), c, state,
		         "ERR", now[i].errors ? HUD_C_BAD : HUD_C_DIM,
		         (unsigned)now[i].errors,
		         s.verify ? " " : "", HUD_C_WARN, s.verify ? "VERIFY" : "");
	}
	n += hud_ra_lines(&L[n], VM_HUD_LINES - n, &fg, shown);
	if (shown && n < VM_HUD_LINES)
		n += rt_line(L[n]);
	if (vm_hud_installed())
		vm_hud_set_text(L, shown ? n : 0);
}


/* ---- the overlay (SELECT + arrows) ------------------------------------------ */

static const char *const k_ov_ra[] = { "Off", "On", "Adaptive" };   /* VM_RA_* */
static const char *const k_ov_off[] = { "Off", "?", "Cached only", "C + NC R/A", "C + NC All" };
static const char *const k_ov_pipe[] = { "Off", "Auto", "Fixed" };
/* GPU-safe reads, by value (VM_GPUSAFE_*) */
static const char *const k_ov_gs[] = { "Off", "All", "Non-DMA" };

/* The read ahead test's line in the HUD, while it runs. */
static int rt_line(char *o)
{
	uint32_t mib = 0;

	if (!vm_stage_ratest_running(&mib))
		return 0;
	snprintf(o, VM_HUD_LINE_MAX, HUD_C_ACCENT "R/A Test: " HUD_C_TEXT "%u MiB", (unsigned)mib);
	return 1;
}

/* Start (copy on core 3 goes to C + NC R/A meanwhile), or stop and put in what it
 * found, saved in the app's profile. `ra` the read ahead in effect: Adaptive
 * stays Adaptive (from the thresholds found) when the test keeps it on.
 * *save: to be saved once in effect. */
static int rt_toggle(VmApp *a, int ra, char *msg, int cap, int *save)
{
	static int before;                    /* the copy setting it started from */
	VmRaTestPick p;

	if (!vm_stage_ratest_running(NULL)) {
		const int r = vm_stage_ratest_start();

		if (r == VM_OK) {
			before = a->offload;
			a->offload = VM_OFFLOAD_ALL;      /* C + NC R/A: All only costs time */
			snprintf(msg, cap, HUD_C_GOOD "Test on: play, SEL+> shows the HUD");
		}
		return r;
	}
	vm_stage_ratest_stop(&p);
	if (p.little) {
		if (a->offload == VM_OFFLOAD_ALL)     /* as the test set it, not changed since */
			a->offload = before;
		snprintf(msg, cap, HUD_C_WARN "R/A Test: %u MiB is too little, play more",
		         (unsigned)(p.mib % 100000u));
		return VM_OK;
	}
	a->ra = p.ra == VM_RA_ON && ra == VM_RA_ADAPTIVE ? VM_RA_ADAPTIVE : p.ra;
	a->offload = p.offload;
	a->thr[0] = p.ra == VM_RA_ON ? p.thr[0] : 0;
	a->thr[1] = p.ra == VM_RA_ON ? p.thr[1] : 0;
	*save = 1;
	return VM_OK;
}

/* Overlapped reads and GPU-safe reads: shown on asking. */
static int g_ov_adv;

/* A change asked for in the overlay, made here: the app in front's own
 * settings (in effect at once; kept only by Save), or an action. */
static void overlay_op(uint32_t op, char *msg, int cap)
{
	static const int ra[] = { VM_APP_GLOBAL, VM_RA_OFF, VM_RA_ON, VM_RA_ADAPTIVE };
	static const int off[] = { VM_APP_GLOBAL, VM_OFFLOAD_OFF, VM_OFFLOAD_ALWAYS, VM_OFFLOAD_ALL,
	                           VM_OFFLOAD_FULL };   /* as on the Settings page */
	static const int pipe[] = { VM_APP_GLOBAL, VM_PIPE_OFF, VM_PIPE_AUTO, VM_PIPE_FIXED };
	static const int gs[] = { VM_APP_GLOBAL, VM_GPUSAFE_OFF, VM_GPUSAFE_NODMA, VM_GPUSAFE_ALL };
	static const int thr[] = { 0, 2, 3, 4, 5, 6, 7, 8, VM_RA_NEVER };   /* 0: the fixed */
	const int dir = OV_OP_DIR(op), item = OV_OP_ITEM(op);
	VmAppNow an;
	VmApp a;
	int r = VM_OK, save = 0;

	msg[0] = 0;
	if (item == OV_ITEM_ADV) {
		g_ov_adv = !g_ov_adv;
		return;
	}
	vm_stage_app_now(&an);
	a = an.cur;
	switch (item) {
	case OV_ITEM_RA:
		a.ra = ov_step(ra, 4, a.ra, dir);
		break;
	case OV_ITEM_C:
	case OV_ITEM_NC: {
		const int k = item == OV_ITEM_NC;

		a.thr[k] = (uint32_t)ov_step(thr, 9, (int)a.thr[k], dir);
		break;
	}
	case OV_ITEM_OFFLOAD:
		a.offload = ov_step(off, 5, a.offload, dir);
		break;
	case OV_ITEM_PIPE:
		a.pipe = ov_step(pipe, 4, a.pipe, dir);
		break;
	case OV_ITEM_GPUSAFE:
		a.gpusafe = ov_step(gs, 4, a.gpusafe, dir);
		break;
	case OV_ITEM_TEST:
		r = rt_toggle(&a, a.ra == VM_APP_GLOBAL ? an.global.ra : a.ra, msg, cap, &save);
		break;
	case OV_ITEM_SAVE:
		r = vm_stage_app_save();
		if (r == VM_OK)
			snprintf(msg, cap, HUD_C_GOOD "%.15s: saved", an.title);
		break;
	default:
		return;
	}
	if (item != OV_ITEM_SAVE)
		r = vm_stage_app_set(&a);
	if (r == VM_OK && save) {
		r = vm_stage_app_save();
		if (r == VM_OK)
			snprintf(msg, cap, HUD_C_GOOD "%.15s: picked and saved", an.title);
	}
	if (r == VM_ERR_NOT_READY)
		snprintf(msg, cap, HUD_C_WARN "No app in front");
	else if (r == VM_ERR_IO)
		snprintf(msg, cap, HUD_C_BAD "Not saved (the card?)");
	else if (r != VM_OK)
		snprintf(msg, cap, HUD_C_WARN "Not done (%d)", r);
}

/* The colour of an app's value: changed and not saved blue (as PSVshell marks its
 * own values); saved and not the standard one green; else white. */
static const char *ov_color(int cur, int saved, int standard)
{
	if (cur != saved)
		return HUD_C_ACCENT;
	return cur == VM_APP_GLOBAL || cur == standard ? HUD_C_TEXT : HUD_C_GOOD;
}

/* "Global (Auto)" for a value that is the global one, else the value. */
static void ov_val(char *o, int cap, int cur, int global, const char *const *names, int n)
{
	const int v = cur == VM_APP_GLOBAL ? global : cur;
	const char *name = v >= 0 && v < n ? names[v] : "?";

	if (cur == VM_APP_GLOBAL)
		snprintf(o, cap, "Global (%s)", name);
	else
		snprintf(o, cap, "%s", name);
}

/* One menu line: the cursor, the name padded, the value in its colour. */
static void ov_line(char *o, int cur, int i, int indent, const char *name, const char *color,
                    const char *val)
{
	char nm[24];

	pad(nm, sizeof(nm), name, 20 - 2 * indent, 20, 0);
	snprintf(o, VM_HUD_LINE_MAX, "%s%s%s" HUD_C_TEXT "%s%s%.24s",
	         i == cur ? HUD_C_ACCENT : HUD_C_DIM, i == cur ? "> " : "  ",
	         indent ? "  " : "", nm, color, val);
}

/* Characters that take room (colour codes do not). */
static int ov_vis(const char *s)
{
	int n = 0;

	for (; *s; s++)
		if ((unsigned char)*s > HUD_C_LAST)
			n++;
	return n;
}

/* A line with `text` in the middle of `width`, between > < under the cursor. */
static void ov_center(char *o, int width, int under, const char *color, const char *text)
{
	const int len = ov_vis(text) + 4;
	int left = width > len ? (width - len) / 2 : 0;

	if (left > VM_HUD_LINE_MAX / 2)
		left = VM_HUD_LINE_MAX / 2;
	for (int i = 0; i < left; i++)
		o[i] = ' ';
	snprintf(o + left, (size_t)(VM_HUD_LINE_MAX - left), "%s%s%s%s", under ? HUD_C_ACCENT "> " : "  ",
	         color, text, under ? HUD_C_ACCENT " <" : "  ");
}

static void overlay_feed(void)
{
	static char L[VM_HUD_LINES][VM_HUD_LINE_MAX];
	static int shown;                     /* the overlay is on screen         */
	static uint32_t base_core[4], base_ncs, base_nco;
	static char msg[VM_HUD_LINE_MAX];
	static uint32_t msg_us;
	VmSettings s;
	uint32_t op;
	int n = 0;

	vm_settings_copy(&s);
	vm_overlay_enable(s.overlay && vm_hud_installed(), g_core->wake);
	op = vm_overlay_take_op();
	if (op) {
		overlay_op(op, msg, sizeof(msg));
		msg_us = ksceKernelGetSystemTimeLow();
	}
	if (vm_stage_ratest_stale()) {            /* it was for another app */
		vm_stage_ratest_cancel();
		snprintf(msg, sizeof(msg), HUD_C_WARN "R/A Test: another app, stopped");
		msg_us = ksceKernelGetSystemTimeLow();
	}
	if (vm_overlay_state() != OV_MENU) {
		if (shown) {
			count_cores_want(&g_cnt_ov, 0);
			vm_hud_set_overlay(L, 0);
			shown = 0;
		}
		return;
	}
	if (!shown) {                         /* just opened: count from now */
		count_cores_want(&g_cnt_ov, 1);
		vm_stage_core_stats(base_core, &base_ncs, &base_nco);
		msg[0] = 0;
		shown = 1;
	}
	{
		VmAppNow an;
		VmFg fg;
		uint32_t core[4], ncs, nco, other = 0, mask;
		char a[16], b[16], v[40];
		const int cur = vm_overlay_cursor();
		int ra, save_line = -1, close_line;

		vm_stage_app_now(&an);
		vm_stage_fg(&fg);
		vm_stage_core_stats(core, &ncs, &nco);
		ra = an.cur.ra == VM_APP_GLOBAL ? an.global.ra : an.cur.ra;   /* in effect */
		for (int i = 0; i < 3; i++)
			other += core[i] - base_core[i];
		pct_text(a, sizeof(a), ov_pct(core[3] - base_core[3], other));
		pct_text(b, sizeof(b), ov_pct(ncs - base_ncs, nco - base_nco));
		snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_ACCENT "vita-memio" HUD_C_DIM
		         "                SEL+> HUD");
		{
			VmRaNow rn;

			vm_stage_ra_now(&rn);
			/* a title id is 9 characters ("home screen" 11): the line fits */
			snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_TEXT "%.11s" HUD_C_DIM " %s" HUD_C_DIM
			         "  core 3 %s " HUD_C_DIM "C %.7s" HUD_C_DIM " NC %.7s",
			         an.title[0] ? an.title : "home screen",
			         fg.dev == VM_DEV_SD2VITA ? "SD2Vita" : fg.dev == VM_DEV_MEMCARD ? "memory card"
			         : "", hud_bufmap(rn.bufmap), a, b);
		}
		if (!an.title[0]) {
			mask = 1u << OV_ITEM_CLOSE;
			snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "No app in front: its settings are");
			snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "set while it runs.");
		} else {
			mask = (1u << OV_ITEM_RA) | (1u << OV_ITEM_OFFLOAD) | (1u << OV_ITEM_TEST) |
			       (1u << OV_ITEM_SAVE) | (1u << OV_ITEM_CLOSE);
			snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "-- Settings (this app) --");
			{
				uint32_t mib = 0;
				const int run = vm_stage_ratest_running(&mib);

				if (run)
					snprintf(v, sizeof(v), "(X) %u MiB", (unsigned)mib);
				ov_line(L[n++], cur, OV_ITEM_TEST, 0, run ? "Stop R/A Test" : "Start R/A Test",
				        HUD_C_TEXT, run ? v : "(X)");
			}
			ov_val(v, sizeof(v), an.cur.ra, an.global.ra, k_ov_ra, 3);
			ov_line(L[n++], cur, OV_ITEM_RA, 0, "Read ahead",
			        ov_color(an.cur.ra, an.saved.ra, an.global.ra), v);
			if (ra != VM_RA_OFF) {                /* its thresholds: On and Adaptive */
				VmRaNow rn;

				vm_stage_ra_now(&rn);             /* Adaptive: where it is now */
				for (int k = 0; k < 2; k++) {
					const uint32_t t = an.cur.thr[k] ? an.cur.thr[k] : an.global.thr[k];

					if (t == VM_RA_NEVER)
						snprintf(v, sizeof(v), "never");
					else if (ra == VM_RA_ADAPTIVE && rn.thr[k] != t)
						snprintf(v, sizeof(v), "%u%s, now %u", (unsigned)t,
						         an.cur.thr[k] ? "" : " (fixed)", (unsigned)rn.thr[k]);
					else
						snprintf(v, sizeof(v), "%u%s", (unsigned)t,
						         an.cur.thr[k] ? "" : " (fixed)");
					ov_line(L[n++], cur, k ? OV_ITEM_NC : OV_ITEM_C, 1,
					        k ? "NC streak" : "C streak",
					        an.cur.thr[k] != an.saved.thr[k] ? HUD_C_ACCENT
					        : an.cur.thr[k] && an.cur.thr[k] != an.global.thr[k] ? HUD_C_GOOD
					        : HUD_C_TEXT, v);
				}
				mask |= (1u << OV_ITEM_C) | (1u << OV_ITEM_NC);
			}
			ov_val(v, sizeof(v), an.cur.offload, an.global.offload, k_ov_off, 5);
			ov_line(L[n++], cur, OV_ITEM_OFFLOAD, 0, "Copy on core 3",
			        ov_color(an.cur.offload, an.saved.offload, an.global.offload), v);
			ov_line(L[n++], cur, OV_ITEM_ADV, 0, g_ov_adv ? "Hide advanced" : "Show advanced",
			        HUD_C_TEXT, "(X)");
			mask |= 1u << OV_ITEM_ADV;
			if (g_ov_adv) {
				mask |= (1u << OV_ITEM_PIPE) | (1u << OV_ITEM_GPUSAFE);
				ov_val(v, sizeof(v), an.cur.pipe, an.global.pipe, k_ov_pipe, 3);
				ov_line(L[n++], cur, OV_ITEM_PIPE, 1, "Overlapped reads",
				        ov_color(an.cur.pipe, an.saved.pipe, an.global.pipe), v);
				ov_val(v, sizeof(v), an.cur.gpusafe, an.global.gpusafe, k_ov_gs, 3);
				ov_line(L[n++], cur, OV_ITEM_GPUSAFE, 1, "GPU-safe reads",
				        ov_color(an.cur.gpusafe, an.saved.gpusafe, an.global.gpusafe), v);
			}
			save_line = n++;
		}
		close_line = n++;
		vm_overlay_set_mask(mask);
		if (msg[0] && ksceKernelGetSystemTimeLow() - msg_us < 4u * 1000u * 1000u)
			snprintf(L[n++], VM_HUD_LINE_MAX, "%s", msg);
		else
			snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "Up/Down  Left/Right change  X do");
		/* Save and Close: in the middle of the panel, whatever is in it */
		{
			int width = 0;

			for (int i = 0; i < n; i++)
				if (i != save_line && i != close_line && ov_vis(L[i]) > width)
					width = ov_vis(L[i]);
			if (save_line >= 0) {
				const int dirty = memcmp(&an.cur, &an.saved, sizeof(an.cur)) != 0;

				ov_center(L[save_line], width, cur == OV_ITEM_SAVE,
				          dirty ? HUD_C_ACCENT : HUD_C_TEXT, dirty ? "Save (not saved)" : "Save");
			}
			ov_center(L[close_line], width, cur == OV_ITEM_CLOSE, HUD_C_TEXT, "Close");
		}
	}
	vm_hud_set_overlay(L, n);
}
/* ---- the calls from the kernel module's worker ---------------------------------- */

static void hud_tick(const VmDevState now[VM_DEV_COUNT])
{
	hud_feed(now);
	overlay_feed();
}

/* What the worker should wake for: the overlay's live values, or the HUD's
 * speeds. With the overlay switched on, the HUD shows only by its keys (see
 * hud_effective); otherwise as its setting says. */
static int hud_busy(void)
{
	VmSettings s;

	if (vm_overlay_state() == OV_MENU)
		return VM_HUD_MENU;
	vm_settings_copy(&s);
	if (s.overlay && vm_overlay_installed())
		return vm_overlay_state() == OV_HUD ? VM_HUD_SHOWN : VM_HUD_IDLE;
	return VM_HUD_IDLE;                   /* the HUD is the overlay's only */
}

static void hud_fini_all(void)
{
	vm_overlay_fini();
	vm_hud_fini();
}

/* ---- module lifecycle ------------------------------------------------------------ */

int module_start(SceSize argc, const void *args);
/* Entry symbol for the linker; taiHEN itself enters through module_start. */
int _start(SceSize argc, const void *args) __attribute__ ((weak, alias("module_start")));

int module_start(SceSize argc, const void *args)
{
	VmHudLink *l;

	/* Only the kernel module of the same build may load it (hud_link.h). */
	if (argc != sizeof(VmHudLink *) || !args)
		return SCE_KERNEL_START_NO_RESIDENT;
	l = *(VmHudLink *const *)args;
	if (!l || l->magic != VM_HUD_LINK_MAGIC || l->version != VM_HUD_LINK_VERSION || !l->core ||
	    l->core->size != sizeof(VmCoreApi) || !l->core->build ||
	    strncmp(l->core->build, VM_BUILD_STR, sizeof(VM_BUILD_STR)) != 0)
		return SCE_KERNEL_START_NO_RESIDENT;
	g_core = l->core;
	l->hud.tick = hud_tick;
	l->hud.busy = hud_busy;
	l->hud.fini = hud_fini_all;
	__sync_synchronize();
	l->hud.size = sizeof(VmHudApi);   /* last: the kernel module checks it */
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	if (g_core)
		hud_fini_all();
	return SCE_KERNEL_STOP_SUCCESS;
}
