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
 *     saves settings, allocates the verify buffer on demand, loads the HUD
 *     module and lets it draw (hud_link.c: vitamemio_hud.skprx, loaded only
 *     once the HUD or the overlay is first switched on), logs problems,
 *     follows the logging switch, and clears the boot guard once the home
 *     screen (SceShell) has been up for 30 s;
 *   - publish the user-callable "SceVitaMemio" syscall library (see
 *     include/vitamemio_kernel_api.h and vitamemio_kernel.yml).
 *
 * The exported functions are the user->kernel boundary. The state is copied out
 * with ksceKernelMemcpyKernelToUser, which fails cleanly on a bad pointer; key
 * ids and values are validated before anything changes. They do no file
 * I/O and no logging themselves - the worker saves and logs within a second.
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
#include "hud_link.h"

#define VM_GUARD_PATH  VM_DATA_DIR "/boot_guard"
#include "guard.h"
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
 *   - also woken when an app comes to the front (its own settings);
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

static uint32_t uptime_s(void)
{
	return (uint32_t)(((uint64_t)ksceKernelGetSystemTimeWide() - g_t0_us) / 1000000u);
}

static char g_guard_txt[48];   /* last content read, for the log */
static int  g_guard_len;       /* its length: more than 1 from an older version */

/* First byte of the guard file, or 0 if there is none. */
static char guard_read(void)
{
	return vm_guard_read(VM_GUARD_PATH, g_guard_txt, (int)sizeof(g_guard_txt), &g_guard_len);
}

/* VM_OK, or VM_ERR_IO if the file could not be written. */
static int guard_write(char c)
{
	return vm_guard_write(VM_GUARD_PATH, VM_DATA_DIR, c, &g_guard_len) < 0 ? VM_ERR_IO : VM_OK;
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
		guard_write('0');
		g_guard_done = 1;
	} else if (up < 0 && g_uptime >= VM_GUARD_FALLBACK_SECS) {
		guard_write('0');
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

static const char *const k_dev_log[VM_DEV_COUNT] = { "sd2vita", "internal", "memcard" };

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
		const int hud = running ? vm_hud_link_busy() : VM_HUD_IDLE;   /* hud_link.h */
		SceUInt wait = 0;

		if (hud == VM_HUD_MENU)
			wait = VM_WORKER_OVERLAY_US;
		else if (running && (!g_guard_done || hud == VM_HUD_SHOWN))
			wait = VM_WORKER_FAST_US;
		else if (vm_settings_pending() || vm_stage_pending())
			wait = VM_WORKER_RETRY_US;      /* a save or a helper release to retry */
		else if (__atomic_load_n(&g_vm_settings.log, __ATOMIC_RELAXED) && running)
			wait = VM_WORKER_LOG_US;
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
		vm_hud_link_tick(now, worker_wake);
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

int vmGetDma(VmDma *out)
{
	VmDma k;

	if (!out)
		return VM_ERR_ARG;
	vm_stage_dma(&k);
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
	return guard_write('0');
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
	g_worker = ksceKernelCreateThread("SceVitaMemioWorker", worker, 0x80, VM_STACK_WORKER, 0,
	                                  VM_CPU_SYSTEM, NULL);
	if (g_worker < 0)
		g_worker = ksceKernelCreateThread("SceVitaMemioWorker", worker, 0x80, VM_STACK_WORKER, 0, 0,
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
	 * Reset safe mode (vmResetSafeMode) clears it. One byte written in place,
	 * never deleted (guard.h). */
	if (guard_read() == '1') {
		g_disabled = VM_DIS_SAFE;
		VM_KLOGW("boot_guard is 1 [%s] - safe mode, nothing installed (reset it in "
		         "Settings > Devices > vita-memio > Advanced)", g_guard_txt);
		start_worker();                   /* settings can still be changed */
		return SCE_KERNEL_START_SUCCESS;
	}
	guard_write('1');
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
	if (g_worker >= 0) {
		ksceKernelWaitThreadEnd(g_worker, NULL, NULL);
		ksceKernelDeleteThread(g_worker);
		g_worker = -1;
	}
	if (g_wake >= 0) {
		ksceKernelDeleteSema(g_wake);
		g_wake = -1;
	}
	vm_hud_link_fini();
	vm_stage_fini();
	vm_settings_flush();
	vm_klog_fini();
	return SCE_KERNEL_STOP_SUCCESS;
}
