/*
 * main.c - SceVitaMemio kernel module entry point.
 *
 * Responsibilities:
 *   - bring up the logger (klog.c);
 *   - check the storage driver is one this build knows (3.60-3.74); on any
 *     other firmware install nothing at all;
 *   - the crash-loop boot guard: if the last start never finished, install
 *     nothing (safe mode);
 *   - allocate the staging buffers and install the I/O hooks (stage.c), then
 *     load and apply the saved settings (settings.c);
 *   - run the worker (every second while the HUD is on or the boot guard
 *     waits, otherwise only when there is something to do, see g_wake):
 *     saves settings, allocates the verify buffer on demand, feeds the HUD
 *     (hud.c, installed only once the HUD is first switched on), logs
 *     problems, follows the logging switch, and clears the boot guard once
 *     the home screen (SceShell) has been up for 30 s;
 *   - publish the user-callable "SceVitaMemio" syscall library (see
 *     include/vitamemio_kernel_api.h and vitamemio_kernel.yml).
 *
 * The exported functions are the user->kernel boundary. The state is copied out
 * with ksceKernelMemcpyKernelToUser, which fails cleanly on a bad pointer; key
 * ids and values are validated before anything changes. They do no file I/O and
 * no logging themselves - the worker saves and logs within a second.
 */
#include <psp2kern/kernel/modulemgr.h>
#include <psp2kern/kernel/sysmem.h>    /* ksceKernelMemcpy{User,Kernel} macros */
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>          /* ksceIoMkdir                          */
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include "../include/vitamemio.h"
#include "../include/vitamemio_kernel_api.h"
#include "klog.h"
#include "settings.h"
#include "stage.h"
#include "hud.h"
#include "hud_i18n.h"
#include "overlay.h"
#include "overlay_logic.h"

#define VM_GUARD_PATH  VM_DATA_DIR "/boot_guard"
/* The start counts as good once the home screen (SceShell) has been up for
 * this many seconds, not as soon as it appears: a setting that
 * makes the console restart shortly after the home screen (the GPU-mapping
 * watch once did, about 10 s in) must also end in safe mode, not in a loop.
 * The price: restarting by hand within this time after the home screen also
 * leads to safe mode once (Reset safe mode ends it). */
#define VM_GUARD_SHELL_TICKS  30
/* Only if SceShell cannot be looked up at all: clear the guard after this long. */
#define VM_GUARD_FALLBACK_SECS  120

/* Storage driver (SceSdif) module fingerprints of the firmwares this build
 * supports. Its code is identical across all of them; anything else stays
 * stock. */
static const uint32_t k_known_fw[] = {
	0x2e7c52f7,   /* 3.60 */
	0x707d9ddd,   /* 3.61 */
	0x10046e19,   /* 3.63 */
	0x285023ed,   /* 3.65 */
	0x555b752a,   /* 3.67 */
	0x65b8a859,   /* 3.68 */
	0x6a9a4d5b,   /* 3.69 */
	0xc39462fe,   /* 3.70 */
	0x70e63db7,   /* 3.71 */
	0x22158017,   /* 3.72 */
	0x74b301ef,   /* 3.73 */
	0xabd91b5b,   /* 3.74 */
};

int module_get_export_func(SceUID pid, const char *modname, uint32_t libnid,
                           uint32_t funcnid, uintptr_t *func);

static int      g_disabled = VM_DIS_NONE;
static uint32_t g_fingerprint;
static SceUID   g_worker = -1;
static volatile int g_run = 1;
static volatile uint32_t g_uptime;   /* seconds since the module started */
static uint64_t g_t0_us;             /* ... from this system time          */

/* The worker sleeps on g_wake until it has something to do:
 *   - every second while the HUD is on (live speeds) or the boot guard waits
 *     for the home screen;
 *   - every 5 s while a failed settings save is to be retried;
 *   - every 250 ms while the overlay is open (its live values);
 *   - once a minute while the log is on (the stage's statistics line);
 *   - every 2 s while read ahead learns a game or looks for its storage
 *     (auto or statistics; also woken when a new game starts reading);
 *   - otherwise not at all, until it is woken: by a changed setting (saved
 *     and applied at once: buffers, helper threads) or by a device error or
 *     verify mismatch (logged at once). */
#define VM_WORKER_FAST_US   (1000u * 1000u)
#define VM_WORKER_RETRY_US  (5u * 1000u * 1000u)
#define VM_WORKER_LOG_US    (60u * 1000u * 1000u)
static SceUID g_wake = -1;
static volatile int g_guard_done;    /* the boot guard is settled            */

#define VM_WORKER_OVERLAY_US (250u * 1000u)

static void worker_wake(void)
{
	if (g_wake >= 0)
		ksceKernelSignalSema(g_wake, 1);   /* already pending: refused, fine */
}

/* Whether the HUD is on screen: with the overlay switched on, only by its
 * keys (see hud_effective); otherwise as its setting says. */
static int hud_shown(void)
{
	if (__atomic_load_n(&g_vm_settings.overlay, __ATOMIC_RELAXED) && vm_overlay_installed())
		return vm_overlay_state() == OV_HUD;
	return __atomic_load_n(&g_vm_settings.hud, __ATOMIC_RELAXED) != VM_HUD_OFF;
}

static uint32_t uptime_s(void)
{
	return (uint32_t)(((uint64_t)ksceKernelGetSystemTimeWide() - g_t0_us) / 1000000u);
}

static char g_guard_txt[48];   /* last content read, for the log */

/* First byte of the guard file, or 0 if there is none. */
static char guard_read(void)
{
	memset(g_guard_txt, 0, sizeof(g_guard_txt));
	SceUID fd = ksceIoOpen(VM_GUARD_PATH, SCE_O_RDONLY, 0);
	if (fd >= 0) {
		ksceIoRead(fd, g_guard_txt, sizeof(g_guard_txt) - 1);
		ksceIoClose(fd);
	}
	for (int i = 0; g_guard_txt[i]; i++)
		if (g_guard_txt[i] < ' ' || g_guard_txt[i] > '~')
			g_guard_txt[i] = '.';
	return g_guard_txt[0];
}

/* VM_OK, or VM_ERR_IO if the file could not be written. */
static int guard_write(char c, const char *who)
{
	char buf[64];
	int n = 0, w = -1;
	buf[n++] = c;
	n += snprintf(buf + n, sizeof(buf) - n, " up=%u by=%s\n",
	              (unsigned)ksceKernelGetSystemTimeLow(), who);
	ksceIoMkdir("ur0:data", 0777);
	ksceIoMkdir(VM_DATA_DIR, 0777);
	SceUID fd = ksceIoOpen(VM_GUARD_PATH, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	if (fd >= 0) {
		w = ksceIoWrite(fd, buf, n);
		ksceIoClose(fd);
	}
	return w == n ? VM_OK : VM_ERR_IO;
}

/* ---- the home screen, for the boot guard ------------------------------------ */

/* ksceKernelSysrootGetShellPid (SceSysrootForDriver), looked up at run time so
 * a firmware without it only loses the early guard clear, not the module. */
static SceUID (*g_shell_pid)(void);

static void shell_lookup_init(void)
{
	uintptr_t f = 0;

	if (module_get_export_func(KERNEL_PID, "SceSysmem", 0x2ED7F97A, 0x05093E7B, &f) >= 0)
		g_shell_pid = (SceUID (*)(void))f;
}

/* 1: SceShell is loaded, 0: not yet, -1: cannot tell on this system. */
static int shell_up(void)
{
	tai_module_info_t info;
	SceUID pid;

	if (!g_shell_pid)
		return -1;
	pid = g_shell_pid();
	if (pid <= 0)
		return 0;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	return taiGetModuleInfoForKernel(pid, "SceShell", &info) >= 0;
}

/* Called by the worker (every second) until the guard is cleared. */
static void guard_tick(void)
{
	static int seen;                 /* the home screen is up ...              */
	static uint32_t since;           /* ... since this uptime                  */
	int up;

	if (g_guard_done)
		return;
	up = shell_up();
	if (up <= 0) {
		seen = 0;
	} else if (!seen) {
		seen = 1;
		since = g_uptime;
	}
	if (seen && g_uptime - since >= VM_GUARD_SHELL_TICKS) {
		guard_write('0', "shell");
		g_guard_done = 1;
	} else if (up < 0 && g_uptime >= VM_GUARD_FALLBACK_SECS) {
		guard_write('0', "timer");
		VM_KLOGW("boot guard: the home screen cannot be detected here, cleared after %us",
		         (unsigned)VM_GUARD_FALLBACK_SECS);
		g_guard_done = 1;
	}
}

static int firmware_known(void)
{
	tai_module_info_t info;

	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	if (taiGetModuleInfoForKernel(KERNEL_PID, "SceSdif", &info) < 0)
		return 0;
	g_fingerprint = info.module_nid;
	for (unsigned i = 0; i < sizeof(k_known_fw) / sizeof(k_known_fw[0]); i++)
		if (k_known_fw[i] == g_fingerprint)
			return 1;
	return 0;
}

/* ---- the worker ------------------------------------------------------------- */

static const char *const k_dev_short[VM_DEV_COUNT] = { "SD2V", "INT ", "MC  " };   /* compact */
static const char *const k_dev_log[VM_DEV_COUNT] = { "sd2vita", "internal", "memcard" };
/* HUD order: the removable storages first. */
static const int k_hud_order[VM_DEV_COUNT] = { VM_DEV_SD2VITA, VM_DEV_MEMCARD, VM_DEV_INTERNAL };

/* System language for the HUD words, from the registry; English until known. */
static int g_lang = 1;

static void read_lang(void)
{
	static int (*get_int)(const char *, const char *, int *) = 0;
	static int tried = 0;
	if (!tried) {
		uintptr_t f = 0;
		tried = 1;
		if (module_get_export_func(KERNEL_PID, "SceRegistryMgr", 0xB2223AEB,
		                           0x16DDF3DC, &f) >= 0)
			get_int = (int (*)(const char *, const char *, int *))f;
	}
	int v = 1;
	if (get_int && get_int("/CONFIG/SYSTEM", "language", &v) >= 0)
		g_lang = v;
}

/* "12.3" from bytes per second. */
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
static void pad(char *o, int cap, const char *s, int width, int max, int right)
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

/* With the overlay switched on, the HUD shows only by its keys (SELECT +
 * Right from the overlay), in the PSVshell style, as the HUD setting says
 * (Extended while that is Off). */
static void hud_effective(VmSettings *s)
{
	if (!s->overlay || !vm_overlay_installed())
		return;
	if (vm_overlay_state() == OV_HUD) {
		if (s->hud == VM_HUD_OFF)
			s->hud = VM_HUD_EXTENDED;
		s->hud_style = VM_HUD_STYLE_PSVSHELL;
	} else {
		s->hud = VM_HUD_OFF;
	}
}

static void hud_feed(const VmDevState *now)
{
	static uint64_t last_rd[VM_DEV_COUNT], last_wr[VM_DEV_COUNT], last_us;
	static char L[VM_HUD_LINES][VM_HUD_LINE_MAX];
	static int hud_failed;           /* tried once and failed: do not retry */
	const uint64_t now_us = (uint64_t)ksceKernelGetSystemTimeWide();
	const uint32_t ms = last_us ? (uint32_t)((now_us - last_us) / 1000u) : 1000u;
	VmSettings s;
	int n = 0;

	last_us = now_us;

	vm_settings_copy(&s);
	if (s.hud != VM_HUD_OFF || s.overlay)
		read_lang();
	/* the overlay draws through the HUD's hook too */
	if ((s.hud != VM_HUD_OFF || s.overlay) && !vm_hud_installed() && !hud_failed &&
	    vm_hud_init() != VM_OK) {
		hud_failed = 1;
		VM_KLOGW("hud: could not install - HUD unavailable");
	}
	hud_effective(&s);
	if (!vm_hud_installed())
		s.hud = VM_HUD_OFF;
	/* One line per storage that has seen any I/O since boot (an SD2Vita line
	 * only appears with one fitted): live speed of all its transfers, whether
	 * or not they took the faster path, coloured by what the faster path is
	 * doing. Extended adds totals and state. */
	int slow = vm_stage_memcard_slow() == 1;

	for (int o = 0; o < VM_DEV_COUNT; o++) {
		static uint64_t seen[VM_DEV_COUNT];
		int i = k_hud_order[o];
		uint64_t rd, wr;
		char r[12], w[12], rp[12], wp[12], np[16];

		vm_stage_bytes(i, &rd, &wr);
		mbps(r, sizeof(r), rd - last_rd[i], ms);
		mbps(w, sizeof(w), wr - last_wr[i], ms);
		last_rd[i] = rd;
		last_wr[i] = wr;
		seen[i] |= rd | wr;
		if (s.hud == VM_HUD_OFF || !seen[i])
			continue;
		int dev_slow = i == VM_DEV_MEMCARD && slow;
		const char *c = state_color(&now[i], dev_slow);
		const char *name = s.hud == VM_HUD_COMPACT ? k_dev_short[i]
		                 : i == VM_DEV_INTERNAL ? hud_tr(g_lang, HTR_INTERNAL)
		                 : i == VM_DEV_MEMCARD  ? hud_tr(g_lang, HTR_MEMCARD) : "SD2VITA";
		const char *flag = now[i].status == VM_ST_FALLBACK ? hud_tr(g_lang, HTR_ERROR)
		                 : dev_slow ? hud_tr(g_lang, HTR_SLOW) : "";
		pad(np, sizeof(np), name, s.hud == VM_HUD_COMPACT ? 4 : 8, 14, 0);
		pad(rp, sizeof(rp), r, 5, 6, 1);
		pad(wp, sizeof(wp), w, 5, 6, 1);
		snprintf(L[n++], VM_HUD_LINE_MAX,
		         HUD_C_TEXT "%s " HUD_C_DIM "R %s%s " HUD_C_DIM "W %s%s " HUD_C_DIM "MB/s%s%s%s",
		         np, c, rp, c, wp, flag[0] ? " " : "", c, flag);
		if (s.hud != VM_HUD_EXTENDED)
			continue;
		const char *state =
			now[i].status == VM_ST_ACTIVE   ? hud_tr(g_lang, HTR_ON) :
			now[i].status == VM_ST_OFF      ? hud_tr(g_lang, HTR_OFF) :
			now[i].status == VM_ST_FALLBACK ? hud_tr(g_lang, HTR_STANDARD) :
			                                  hud_tr(g_lang, HTR_UNAVAILABLE);
		snprintf(L[n++], VM_HUD_LINE_MAX,
		         HUD_C_DIM "  %u/%u MiB %s%s " HUD_C_DIM "%s %s%u%s%s%s",
		         (unsigned)(rd >> 20), (unsigned)(wr >> 20), c, state,
		         hud_tr(g_lang, HTR_ERR), now[i].errors ? HUD_C_BAD : HUD_C_DIM,
		         (unsigned)now[i].errors,
		         s.verify ? " " : "", HUD_C_WARN, s.verify ? hud_tr(g_lang, HTR_VERIFY) : "");
	}
	if (vm_hud_installed())
		vm_hud_set_text(L, s.hud == VM_HUD_OFF ? 0 : n, s.hud_style);
}


/* ---- the overlay (SELECT + arrows) ------------------------------------------ */

static const char *const k_ov_ra[] = { "Off", "On", "Auto" };
static const char *const k_ov_off[] = { "Off", "GPU-safe (C)", "Cached only", "C + NC",
                                        "NCached only" };
static const char *const k_ov_pipe[] = { "Off", "Auto", "Fixed" };

/* A change asked for in the overlay, made here (the worker owns the
 * settings and the files). */
static void overlay_op(uint32_t op, const VmSettings *s, char *msg, int cap)
{
	static const int ra[] = { VM_RA_OFF, VM_RA_ON, VM_RA_AUTO };
	static const int off[] = { VM_OFFLOAD_OFF, VM_OFFLOAD_GPU, VM_OFFLOAD_ALWAYS, VM_OFFLOAD_NC,
	                           VM_OFFLOAD_ALL };   /* as on the Settings page */
	static const int pipe[] = { VM_PIPE_OFF, VM_PIPE_AUTO, VM_PIPE_FIXED };
	static const int onoff[] = { 0, 1 };
	const int dir = OV_OP_DIR(op);

	msg[0] = 0;
	switch (OV_OP_ITEM(op)) {
	case OV_ITEM_RA:
		vm_settings_set(VM_KEY_READAHEAD, ov_step(ra, 3, s->readahead, dir));
		break;
	case OV_ITEM_OFFLOAD:
		vm_settings_set(VM_KEY_OFFLOAD, ov_step(off, 5, s->offload, dir));
		break;
	case OV_ITEM_PIPE:
		vm_settings_set(VM_KEY_PIPELINE, ov_step(pipe, 3, s->pipeline, dir));
		break;
	case OV_ITEM_GPUSAFE:
		vm_settings_set(VM_KEY_GPUSAFE, ov_step(onoff, 2, s->gpusafe, dir));
		break;
	case OV_ITEM_RESET: {
		VmRaNow now;

		vm_stage_ra_now(&now);
		if (!now.title[0])
			snprintf(msg, cap, HUD_C_WARN "No game followed yet");
		else if (vm_stage_ra_delete(now.title) == VM_OK)
			snprintf(msg, cap, HUD_C_GOOD "%s: learning again", now.title);
		else
			snprintf(msg, cap, HUD_C_WARN "%s: no profile yet", now.title);
		break;
	}
	default:
		break;
	}
	vm_settings_flush();                  /* saved now */
}

/* "after N" / "never" / "-" into o. */
static void ov_thr(char *o, int cap, int mode, uint32_t t)
{
	if (mode == VM_RA_OFF)
		snprintf(o, cap, HUD_C_DIM "-");
	else if (t == VM_RA_NEVER)
		snprintf(o, cap, HUD_C_DIM "never");
	else
		snprintf(o, cap, HUD_C_GOOD "%u", (unsigned)t);
}

static void ov_pct_text(char *o, int cap, int p)
{
	if (p < 0)
		snprintf(o, cap, HUD_C_DIM "-");
	else
		snprintf(o, cap, "%s%u%%", p >= 90 ? HUD_C_GOOD : p > 0 ? HUD_C_WARN : HUD_C_BAD,
		         (unsigned)(p > 100 ? 100 : p));
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
	vm_overlay_enable(s.overlay && vm_hud_installed(), worker_wake);
	op = vm_overlay_take_op();
	if (op) {
		overlay_op(op, &s, msg, sizeof(msg));
		msg_us = ksceKernelGetSystemTimeLow();
		vm_settings_copy(&s);
	}
	if (vm_overlay_state() != OV_MENU) {
		if (shown) {
			vm_stage_count_cores(0);
			vm_hud_set_overlay(L, 0);
			shown = 0;
		}
		return;
	}
	if (!shown) {                         /* just opened: count from now */
		vm_stage_count_cores(1);
		vm_stage_core_stats(base_core, &base_ncs, &base_nco);
		msg[0] = 0;
		shown = 1;
	}
	{
		VmRaNow now;
		uint32_t core[4], ncs, nco, other = 0;
		char a[16], b[16];
		const char *dev;
		const int cur = vm_overlay_cursor();

		vm_stage_ra_now(&now);
		vm_stage_core_stats(core, &ncs, &nco);
		dev = now.dev == VM_DEV_SD2VITA ? "SD2Vita" : now.dev == VM_DEV_MEMCARD ? "memory card"
		    : "?";
		snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_ACCENT "vita-memio" HUD_C_DIM
		         "   SEL+> HUD  SEL+< close");
		snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "Game " HUD_C_TEXT "%s" HUD_C_DIM
		         "  storage " HUD_C_TEXT "%s", now.title[0] ? now.title : "-", dev);
		if (!now.measure)
			snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "Auto: off (Read ahead %s)",
			         k_ov_ra[s.readahead]);
		else if (!now.title[0])
			snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "Auto: waiting for a game");
		else if (now.learning)
			snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "Auto: " HUD_C_WARN "learning %u/300",
			         (unsigned)now.streams);
		else
			snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "Auto: " HUD_C_GOOD "learnt");
		ov_thr(a, sizeof(a), now.mode, now.thr[0]);
		ov_thr(b, sizeof(b), now.mode, now.thr[1]);
		snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "Starts after  C %s" HUD_C_DIM "  NC %s"
		         HUD_C_DIM " reads", a, b);
		for (int i = 0; i < 3; i++)
			other += core[i] - base_core[i];
		ov_pct_text(a, sizeof(a), ov_pct(core[3] - base_core[3], other));
		ov_pct_text(b, sizeof(b), ov_pct(ncs - base_ncs, nco - base_nco));
		snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "On core 3    C %s" HUD_C_DIM "  NC %s",
		         a, b);
		{
			const char *const names[OV_ITEMS] = { "Read ahead", "Copy on core 3",
			                                      "Overlapped reads", "GPU-safe reads",
			                                      "Reset Auto profile" };
			const char *vals[OV_ITEMS] = {
				k_ov_ra[s.readahead], k_ov_off[s.offload], k_ov_pipe[s.pipeline],
				s.gpusafe ? "On" : "Off", "(X)",
			};

			for (int i = 0; i < OV_ITEMS; i++) {
				char nm[20];

				pad(nm, sizeof(nm), names[i], 18, 18, 0);
				snprintf(L[n++], VM_HUD_LINE_MAX, "%s%s" HUD_C_TEXT "%s%s%s",
				         i == cur ? HUD_C_ACCENT : HUD_C_DIM, i == cur ? "> " : "  ", nm,
				         i == cur ? HUD_C_ACCENT : HUD_C_GOOD, vals[i]);
			}
		}
		if (msg[0] && ksceKernelGetSystemTimeLow() - msg_us < 4u * 1000u * 1000u)
			snprintf(L[n++], VM_HUD_LINE_MAX, "%s", msg);
		else
			snprintf(L[n++], VM_HUD_LINE_MAX, HUD_C_DIM "Up/Down  Left/Right change  X");
	}
	vm_hud_set_overlay(L, n);
}

/* Only problems are logged: a device that went back to stock after an error,
 * or new errors / verify mismatches. Its counters go with it. */
static void log_problem(const VmDevState *d, int i)
{
	VM_KLOGW("dev %s: %s errors=%u mismatches=%u reads=%u (%u MiB) writes=%u (%u MiB) "
	         "verified=%u", k_dev_log[i],
	         d->status == VM_ST_FALLBACK ? "FALLBACK (stock after an error)" : "error",
	         (unsigned)d->errors, (unsigned)d->mismatches, (unsigned)d->reads,
	         (unsigned)d->read_mib, (unsigned)d->writes, (unsigned)d->write_mib,
	         (unsigned)d->verified);
}

static int worker(SceSize args, void *argp)
{
	VmDevState prev[VM_DEV_COUNT];

	(void)args; (void)argp;
	memset(prev, 0, sizeof(prev));
	while (g_run) {
		VmDevState now[VM_DEV_COUNT];
		const int running = g_disabled == VM_DIS_NONE || g_disabled == VM_DIS_HOOKS;
		SceUInt wait = 0;

		if (running && vm_overlay_state() == OV_MENU)
			wait = VM_WORKER_OVERLAY_US;
		else if (running && (!g_guard_done || hud_shown()))
			wait = VM_WORKER_FAST_US;
		else if (vm_settings_pending() || vm_stage_pending())
			wait = VM_WORKER_RETRY_US;      /* a save or a helper release to retry */
		else if (__atomic_load_n(&g_vm_settings.log, __ATOMIC_RELAXED) && running)
			wait = VM_WORKER_LOG_US;
		if (running) {
			const SceUInt t = vm_stage_tick_us();   /* read ahead learning a game */

			if (t && (!wait || t < wait))
				wait = t;
		}
		if (g_wake >= 0)
			ksceKernelWaitSema(g_wake, 1, wait ? &wait : NULL);   /* 0: until woken */
		else
			ksceKernelDelayThread(VM_WORKER_FAST_US);
		if (!g_run)
			break;
		g_uptime = uptime_s();
		/* Always, also in safe mode or on an unknown firmware: settings can
		 * still be changed there (to switch off what broke the last start),
		 * so they are saved, and the log follows its switch. */
		vm_settings_flush();
		vm_klog_set_enabled(__atomic_load_n(&g_vm_settings.log, __ATOMIC_RELAXED));   /* follows the switch */
		/* Safe mode stays until it is reset, and an unknown firmware never
		 * set the guard: nothing else runs in those two. */
		if (g_disabled == VM_DIS_SAFE || g_disabled == VM_DIS_FIRMWARE)
			continue;
		guard_tick();
		vm_stage_worker_tick();
		vm_stage_get(now, 1);
		hud_feed(now);
		overlay_feed();
		for (int i = 0; i < VM_DEV_COUNT; i++) {
			int fell_back = now[i].status == VM_ST_FALLBACK && prev[i].status != VM_ST_FALLBACK;
			int new_err = now[i].errors != prev[i].errors ||
			              now[i].mismatches != prev[i].mismatches;

			if (fell_back || new_err)
				log_problem(&now[i], i);
			prev[i] = now[i];
		}
	}
	return 0;
}

/* ---- exported SceVitaMemio API (user-callable) --------------------------- */

int vmGetState(VmState *out)
{
	VmState k;
	if (!out)
		return VM_ERR_ARG;
	memset(&k, 0, sizeof(k));
	k.size = sizeof(k);
	k.magic = VM_STATE_MAGIC;
	{
		VmSettings cur;

		vm_settings_copy(&cur);
		memcpy(&k.settings, &cur, sizeof(k.settings));   /* the first seven keys */
	}
	k.disabled = g_disabled;
	k.fingerprint = g_fingerprint;
	k.verify_ready = vm_stage_verify_ready();
	k.uptime_s = uptime_s();
	k.memcard_slow = vm_stage_memcard_slow();
	vm_stage_get(k.dev, g_disabled == VM_DIS_NONE);
	if (ksceKernelMemcpyKernelToUser(out, &k, sizeof(k)) < 0)
		return VM_ERR_ARG;
	return VM_OK;
}

/* Accepted in every state, safe mode included: that is when a setting that
 * broke the last start has to be switched off. Without the hooks a change
 * only takes effect at the next start. */
int vmSetSetting(int key, int value)
{
	int r;

	if (!vm_setting_valid(key, value))
		return VM_ERR_ARG;
	r = vm_settings_set(key, value);
	if (r == VM_OK)
		worker_wake();                    /* save and apply it now */
	return r;   /* applies now; worker saves + logs */
}

int vmGetTiming(VmTiming *out)
{
	VmTiming k;

	if (!out)
		return VM_ERR_ARG;
	memset(&k, 0, sizeof(k));
	k.size = sizeof(k);
	k.magic = VM_TIMING_MAGIC;
	vm_stage_timing(k.dev);
	if (ksceKernelMemcpyKernelToUser(out, &k, sizeof(k)) < 0)
		return VM_ERR_ARG;
	return VM_OK;
}

int vmGetSetting(int key)
{
	VmSettings cur;
	int32_t *f;

	vm_settings_copy(&cur);
	f = vm_setting_field(&cur, key);
	return f ? *f : VM_ERR_ARG;
}

int vmGetBuild(char *out, int size)
{
	static const char b[] = VM_BUILD_STR;
	char k[sizeof(b)];
	int n = (int)sizeof(b);

	if (!out || size <= 0)
		return VM_ERR_ARG;
	if (n > size)
		n = size;
	memcpy(k, b, sizeof(b));
	k[n - 1] = 0;
	if (ksceKernelMemcpyKernelToUser(out, k, n) < 0)
		return VM_ERR_ARG;
	return VM_OK;
}

int vmResetSafeMode(void)
{
	return guard_write('0', "settings");
}

/* At most this many profiles per call (a kernel buffer of 64 x 32 bytes). */
#define VM_RA_LIST_MAX 64

int vmRaProfileList(VmRaProfile *out, int max)
{
	static VmRaProfile k[VM_RA_LIST_MAX];   /* one caller at a time: g_list_lock */
	static volatile int lock;
	int n;

	if (!out || max < 0)
		return VM_ERR_ARG;
	if (max > VM_RA_LIST_MAX)
		max = VM_RA_LIST_MAX;
	while (__sync_lock_test_and_set(&lock, 1))
		ksceKernelDelayThread(1000);
	n = vm_stage_ra_list(k, max);
	if (n > 0 && ksceKernelMemcpyKernelToUser(out, k, (SceSize)n * sizeof(k[0])) < 0)
		n = VM_ERR_ARG;
	__sync_lock_release(&lock);
	return n;
}

int vmRaProfileDelete(const char *title)
{
	char t[16];

	if (!title)
		return VM_ERR_ARG;
	memset(t, 0, sizeof(t));
	if (ksceKernelStrncpyUserToKernel(t, title, sizeof(t) - 1) < 0)
		return VM_ERR_ARG;
	t[sizeof(t) - 1] = 0;
	return vm_stage_ra_delete(t);
}

int vmSelfTestPing(void)
{
	return (int)VM_SELFTEST_MAGIC;
}

/* ---- module lifecycle ------------------------------------------------------- */

int module_start(SceSize argc, const void *args);
/* Entry symbol for the linker; taiHEN itself enters through module_start. */
int _start(SceSize argc, const void *args) __attribute__ ((weak, alias("module_start")));

/* SCE_KERNEL_CPU_MASK_SYSTEM: the fourth core, which games do not run on. */
#define VM_CPU_SYSTEM  0x00080000

static void start_worker(void)
{
	g_wake = ksceKernelCreateSema("SceVitaMemioWake", 0, 0, 1, NULL);
	if (g_wake < 0)
		g_wake = -1;                      /* then the worker wakes every second */
	vm_stage_on_problem(worker_wake);     /* errors wake it (no-op without g_wake) */
	/* On the system core, like the overlapped-read helper, so no thread of
	 * the plugin runs on a game's cores; any core if that is refused. */
	g_worker = ksceKernelCreateThread("SceVitaMemioWorker", worker, 0x80, 0x2000, 0,
	                                  VM_CPU_SYSTEM, NULL);
	if (g_worker < 0)
		g_worker = ksceKernelCreateThread("SceVitaMemioWorker", worker, 0x80, 0x2000, 0, 0,
		                                  NULL);
	if (g_worker >= 0)
		ksceKernelStartThread(g_worker, 0, NULL);
	else
		VM_KLOGW("worker thread not started (0x%08x): settings will not be saved",
		         (unsigned)g_worker);
}

int module_start(SceSize argc, const void *args)
{
	(void)argc; (void)args;

	g_t0_us = (uint64_t)ksceKernelGetSystemTimeWide();
	vm_klog_init();   /* FIRST, so the whole bring-up is captured */
	/* The settings next: they say whether anything is logged at all. Applying
	 * them before the hooks exist only sets the per-device switches. */
	vm_settings_init();
	vm_klog_set_enabled(__atomic_load_n(&g_vm_settings.log, __ATOMIC_RELAXED));
	if (!firmware_known()) {
		g_disabled = VM_DIS_FIRMWARE;
		VM_KLOGW("kernel v%s: storage driver 0x%08x not supported - nothing installed",
		         VM_BUILD_STR, (unsigned)g_fingerprint);
		start_worker();                   /* settings can still be changed */
		return SCE_KERNEL_START_SUCCESS;
	}
	/* The one line every start writes: which build, on which storage driver. */
	VM_KLOGI("kernel v%s start, storage driver 0x%08x", VM_BUILD_STR, (unsigned)g_fingerprint);

	/* Crash-loop guard: the file holds '1' from here until the home screen
	 * (SceShell) has been up for VM_GUARD_SHELL_TICKS, then '0'. Finding '1'
	 * at start means the last start never got that far, so this boot installs nothing; Settings > Advanced >
	 * Reset safe mode (vmResetSafeMode) clears it. It is only ever overwritten,
	 * never deleted (a delete can be lost if the console is reset before the
	 * storage flushes). */
	if (guard_read() == '1') {
		g_disabled = VM_DIS_SAFE;
		VM_KLOGW("boot_guard is 1 [%s] - safe mode, nothing installed (reset it in "
		         "Settings > Devices > vita-memio > Advanced)", g_guard_txt);
		start_worker();                   /* settings can still be changed */
		return SCE_KERNEL_START_SUCCESS;
	}
	guard_write('1', "start");
	shell_lookup_init();

	if (vm_stage_init() != VM_OK) {
		g_disabled = VM_DIS_HOOKS;
		VM_KLOGW("storage hooks NOT installed: storage stays stock");
	}
	start_worker();
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	/* Every state runs the worker; the rest releases only what was set up. */
	vm_stage_on_problem(NULL);
	g_run = 0;
	worker_wake();
	if (g_worker >= 0)
		ksceKernelWaitThreadEnd(g_worker, NULL, NULL);
	if (g_wake >= 0) {
		ksceKernelDeleteSema(g_wake);
		g_wake = -1;
	}
	vm_overlay_enable(0, NULL);
	vm_hud_fini();
	vm_stage_fini();
	vm_settings_flush();
	vm_klog_fini();
	return SCE_KERNEL_STOP_SUCCESS;
}
