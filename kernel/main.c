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
 *   - run the 1 Hz worker: saves settings, allocates the verify buffer on
 *     demand, feeds the HUD (hud.c, installed only once the HUD is first
 *     switched on), logs problems, follows the logging switch, and clears the
 *     boot guard once the home screen (SceShell) has been up for 30 s;
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

#define VM_GUARD_PATH  VM_DATA_DIR "/boot_guard"
/* The start counts as good once the home screen (SceShell) has been up for
 * this many worker ticks (seconds), not as soon as it appears: a setting that
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
static volatile uint32_t g_uptime;

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

/* Called once a second until the guard is cleared. */
static void guard_tick(void)
{
	static int done, ticks;
	int up;

	if (done)
		return;
	up = shell_up();
	ticks = up > 0 ? ticks + 1 : 0;
	if (ticks >= VM_GUARD_SHELL_TICKS) {
		guard_write('0', "shell");
		done = 1;
	} else if (up < 0 && g_uptime >= VM_GUARD_FALLBACK_SECS) {
		guard_write('0', "timer");
		VM_KLOGW("boot guard: the home screen cannot be detected here, cleared after %us",
		         (unsigned)VM_GUARD_FALLBACK_SECS);
		done = 1;
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

/* ---- 1 Hz worker ------------------------------------------------------------ */

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
static void mbps(char *o, int n, uint64_t bytes)
{
	uint32_t t = (uint32_t)((bytes * 10) >> 20);   /* tenths of a MiB/s */
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

static void hud_feed(const VmDevState *now)
{
	static uint64_t last_rd[VM_DEV_COUNT], last_wr[VM_DEV_COUNT];
	static char L[VM_HUD_LINES][VM_HUD_LINE_MAX];
	static int hud_failed;           /* tried once and failed: do not retry */
	VmSettings s;
	int n = 0;

	vm_settings_copy(&s);
	if (s.hud != VM_HUD_OFF)
		read_lang();
	if (s.hud != VM_HUD_OFF && !vm_hud_installed() && !hud_failed && vm_hud_init() != VM_OK) {
		hud_failed = 1;
		VM_KLOGW("hud: could not install - HUD unavailable");
	}
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
		mbps(r, sizeof(r), rd - last_rd[i]);
		mbps(w, sizeof(w), wr - last_wr[i]);
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

		ksceKernelDelayThread(1000 * 1000);
		g_uptime++;
		/* Always, also in safe mode or on an unknown firmware: settings can
		 * still be changed there (to switch off what broke the last start),
		 * so they are saved, and the log follows its switch. */
		vm_settings_flush();
		vm_klog_set_enabled(g_vm_settings.log);   /* follows the switch */
		/* Safe mode stays until it is reset, and an unknown firmware never
		 * set the guard: nothing else runs in those two. */
		if (g_disabled == VM_DIS_SAFE || g_disabled == VM_DIS_FIRMWARE)
			continue;
		guard_tick();
		vm_stage_worker_tick();
		vm_stage_get(now, 1);
		hud_feed(now);
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
	k.uptime_s = g_uptime;
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
	if (!vm_setting_valid(key, value))
		return VM_ERR_ARG;
	return vm_settings_set(key, value);   /* applies now; worker saves + logs */
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

int vmResetSafeMode(void)
{
	return guard_write('0', "settings");
}

int vmSelfTestPing(void)
{
	return (int)VM_SELFTEST_MAGIC;
}

/* ---- module lifecycle ------------------------------------------------------- */

int module_start(SceSize argc, const void *args);
/* Entry symbol for the linker; taiHEN itself enters through module_start. */
int _start(SceSize argc, const void *args) __attribute__ ((weak, alias("module_start")));

static void start_worker(void)
{
	g_worker = ksceKernelCreateThread("SceVitaMemioWorker", worker, 0x80, 0x2000, 0, 0, NULL);
	if (g_worker >= 0)
		ksceKernelStartThread(g_worker, 0, NULL);
	else
		VM_KLOGW("worker thread not started (0x%08x): settings will not be saved",
		         (unsigned)g_worker);
}

int module_start(SceSize argc, const void *args)
{
	(void)argc; (void)args;

	vm_klog_init();   /* FIRST, so the whole bring-up is captured */
	/* The settings next: they say whether anything is logged at all. Applying
	 * them before the hooks exist only sets the per-device switches. */
	vm_settings_init();
	vm_klog_set_enabled(g_vm_settings.log);
	if (!firmware_known()) {
		g_disabled = VM_DIS_FIRMWARE;
		VM_KLOGW("kernel v%s: storage driver 0x%08x not supported - nothing installed",
		         VM_VERSION_STR, (unsigned)g_fingerprint);
		start_worker();                   /* settings can still be changed */
		return SCE_KERNEL_START_SUCCESS;
	}
	/* The one line every start writes: which build, on which storage driver. */
	VM_KLOGI("kernel v%s start, storage driver 0x%08x", VM_VERSION_STR, (unsigned)g_fingerprint);

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
	g_run = 0;
	if (g_worker >= 0)
		ksceKernelWaitThreadEnd(g_worker, NULL, NULL);
	vm_hud_fini();
	vm_stage_fini();
	vm_settings_flush();
	vm_klog_fini();
	return SCE_KERNEL_STOP_SUCCESS;
}
