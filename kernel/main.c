/*
 * main.c - SceVitaMemio kernel module entry point.
 *
 * Responsibilities:
 *   - bring up the logger (klog.c);
 *   - check the storage driver is one this build knows (3.60-3.74); on any
 *     other firmware install nothing at all;
 *   - the crash-loop boot guard (guard.h): if the last start never finished,
 *     install nothing (safe mode);
 *   - install the I/O hooks (stage.c) and load the saved settings
 *     (settings.c) - but while the console starts nothing more: every
 *     transfer goes the standard way. The worker brings the plugin on as soon
 *     as the home screen is up (vm_stage_go), and settles the guard after;
 *   - run the worker (every second while the HUD is on or the boot guard
 *     waits, otherwise only when there is something to do, see g_wake):
 *     saves settings, allocates the verify buffer on demand, loads the HUD
 *     module and lets it draw (hud_link.c: vitamemio_hud.skprx, loaded only
 *     once the HUD or the overlay is first switched on), logs problems,
 *     follows the logging switch;
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
#include <psp2kern/display.h>          /* SceDisplayFrameBufInfo               */
#include <taihen.h>

#include "../include/vitamemio.h"
#include "../include/vitamemio_kernel_api.h"
#include "klog.h"
#include "settings.h"
#include "stage.h"
#include "hud_link.h"

#define VM_GUARD_PATH  VM_DATA_DIR "/boot_guard"
#include "guard.h"
#include "vm_hook.h"

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
 *   - every 100 ms while the console starts (to come on as soon as the home
 *     screen is up), then every second until the boot guard is settled (the
 *     plugin has shown it works, guard.h);
 *   - every second while the HUD is on (live speeds);
 *   - every 5 s while a failed settings save is to be retried;
 *   - every 250 ms while the overlay is open (its live values);
 *   - once a minute while the log is on (the stage's statistics line);
 *   - also woken when an app comes to the front (its own settings);
 *   - otherwise not at all, until it is woken: by a changed setting (saved
 *     and applied at once: buffers, helper threads) or by a device error or
 *     verify mismatch (logged at once). */
#define VM_WORKER_FAST_US   (1000u * 1000u)
#define VM_WORKER_BOOT_US   (100u * 1000u)
#define VM_WORKER_RETRY_US  (5u * 1000u * 1000u)
#define VM_WORKER_LOG_US    (60u * 1000u * 1000u)
static SceUID g_wake = -1;
static volatile int g_guard_done;    /* the boot guard is settled            */
static volatile int g_on;            /* came on: the home screen was up      */
static volatile uint32_t g_on_ms;    /* ... at (ms since the module started) */
static int          g_guard_armed;   /* the start's '1' is written           */
static VmGuard      g_guard;         /* worker only                          */

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

static uint32_t uptime_ms(void)
{
	return (uint32_t)(((uint64_t)ksceKernelGetSystemTimeWide() - g_t0_us) / 1000u);
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

/* ---- reset and power off ------------------------------------------------------ */

/* A reset or a power off asked for (VitaShell's or HENkaku's reboot, the
 * system's restart, switching off): before it goes on, every storage back to
 * the standard way with nothing of ours in flight, the settings saved and the
 * log closed - so the system shuts the cards down cleanly. A plain power off
 * cut the power with nothing left to finish; a reset did not, and a transfer
 * of ours caught in it could leave a card needing a hard reset. Suspend (rest
 * mode) is not touched: the plugin goes on after it. */
#define LIB_POWER        0x1082DA7Fu   /* ScePower: the apps' syscalls */
#define LIB_POWER_DRV    0x1590166Fu   /* ScePowerForDriver            */
#define NID_COLD_RESET   0x0442D852u   /* (k)scePowerRequestColdReset  */
#define NID_STANDBY      0x2B7C7CF4u   /* (k)scePowerRequestStandby    */
#define NID_SOFT_RESET   0x2875994Bu   /* kscePowerRequestSoftReset    */

enum { PW_COLD, PW_STANDBY, PW_COLD_DRV, PW_STANDBY_DRV, PW_SOFT_DRV, PW_COUNT };
static const char *const k_pw_name[PW_COUNT] = { "reset", "power off", "reset", "power off",
                                                 "soft reset" };
static SceUID g_pw_hook[PW_COUNT] = { -1, -1, -1, -1, -1 };
static volatile tai_hook_ref_t g_pw_ref[PW_COUNT];
static volatile int g_pw_down;          /* a request is being handled, or went on */
static volatile int g_worker_busy;      /* the worker's round: files may be written */

static int power_request(int which)
{
	const tai_hook_ref_t ref = vm_hook_ref(&g_pw_ref[which]);
	int first, r;

	if (!ref)
		return VM_HOOK_NO_REF;
	first = !__atomic_exchange_n(&g_pw_down, 1, __ATOMIC_SEQ_CST);   /* before the busy check */
	if (first) {   /* a nested one (the apps' call into the driver's) goes straight on */
		int quiet;

		vm_klog_set_sync(1);              /* its line on the card before the reset */
		/* the worker's round finished: no tuning, profile, settings or log
		 * file being written; it writes none from here on */
		for (int t = 0; t < 1000 && __atomic_load_n(&g_worker_busy, __ATOMIC_SEQ_CST); t++)
			ksceKernelDelayThread(1000);
		quiet = vm_settings_down(1);

		VM_KLOGI("power: %s asked for at %us - every storage the standard way%s",
		         k_pw_name[which], (unsigned)uptime_s(),
		         quiet ? "" : " (a transfer still in flight after 1 s)");
		vm_klog_set_enabled(0);           /* the log file closed */
	}
	r = TAI_CONTINUE(int, ref);
	if (first && r < 0) {                 /* refused: the plugin goes on */
		vm_klog_set_enabled(__atomic_load_n(&g_vm_settings.log, __ATOMIC_RELAXED));
		VM_KLOGW("power: %s refused (0x%08x) - the plugin goes on", k_pw_name[which],
		         (unsigned)r);
		vm_settings_down(0);
		__atomic_store_n(&g_pw_down, 0, __ATOMIC_RELEASE);
		worker_wake();
	}
	return r;
}

static int hook_cold(void)        { return power_request(PW_COLD); }
static int hook_standby(void)     { return power_request(PW_STANDBY); }
static int hook_cold_drv(void)    { return power_request(PW_COLD_DRV); }
static int hook_standby_drv(void) { return power_request(PW_STANDBY_DRV); }
static int hook_soft_drv(void)    { return power_request(PW_SOFT_DRV); }

static void power_hooks_init(void)
{
	static const struct { uint32_t lib, nid; int (*fn)(void); } k[PW_COUNT] = {
		{ LIB_POWER, NID_COLD_RESET, hook_cold },
		{ LIB_POWER, NID_STANDBY, hook_standby },
		{ LIB_POWER_DRV, NID_COLD_RESET, hook_cold_drv },
		{ LIB_POWER_DRV, NID_STANDBY, hook_standby_drv },
		{ LIB_POWER_DRV, NID_SOFT_RESET, hook_soft_drv },
	};

	for (int i = 0; i < PW_COUNT; i++) {
		g_pw_ref[i] = 0;
		g_pw_hook[i] = taiHookFunctionExportForKernel(KERNEL_PID, (tai_hook_ref_t *)&g_pw_ref[i],
		                                              "ScePower", k[i].lib, k[i].nid, k[i].fn);
		if (g_pw_hook[i] < 0)
			VM_KLOGW("power: hook %d not installed (0x%08x) - that %s is not prepared for",
			         i, (unsigned)g_pw_hook[i], k_pw_name[i]);
	}
}

static void power_hooks_fini(void)
{
	for (int i = PW_COUNT - 1; i >= 0; i--)
		if (g_pw_hook[i] >= 0) {
			taiHookReleaseForKernel(g_pw_hook[i], g_pw_ref[i]);
			g_pw_hook[i] = -1;
		}
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

/* ksceDisplayGetProcFrameBufInternal (SceDisplayForDriver 0x9FED47AC,
 * 0x3BC165EF in vita-headers db/360), looked up at run time: whether SceShell
 * shows frames yet - the home screen (LiveArea) is up. */
static int (*g_proc_fb)(SceUID pid, int head, int index, SceDisplayFrameBufInfo *info);

static void frames_lookup_init(void)
{
	uintptr_t f = 0;

	if (module_get_export_func(KERNEL_PID, "SceDisplay", 0x9FED47AC, 0x3BC165EF, &f) >= 0 && f)
		g_proc_fb = (int (*)(SceUID, int, int, SceDisplayFrameBufInfo *))f;
}

/* 1: SceShell has a frame on a screen (the built-in one, or HDMI on a PS TV),
 * 0: not yet, -1: cannot tell on this system. */
static int shell_frames(void)
{
	SceUID pid;
	int known = 0;

	if (!g_proc_fb || !g_shell_pid)
		return -1;
	pid = g_shell_pid();
	if (pid <= 0)
		return 0;
	for (int head = 0; head < 2; head++)
		for (int index = 0; index < 2; index++) {
			SceDisplayFrameBufInfo info;

			memset(&info, 0, sizeof(info));
			info.size = sizeof(info);
			if (g_proc_fb(pid, head, index, &info) < 0)
				continue;
			known = 1;
			if (info.paddr && info.framebuf.base && info.framebuf.width)
				return 1;
		}
	return known ? 0 : -1;
}

static const char *const k_guard_why[] = { "", "the home screen shows frames",
                                           "the home screen is loaded (its frames cannot be seen here)",
                                           "the home screen cannot be told here" };

/* Whether the start counts as good (guard.h): VM_GUARD_PROVEN transfers took
 * the faster path, or no storage is on (none set, or back to the standard
 * path after an error). */
static int guard_proven(void)
{
	VmDevState st[VM_DEV_COUNT];
	uint32_t done = 0;
	int on = 0;

	vm_stage_get(st, 1);
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		on |= st[i].status == VM_ST_ACTIVE;
		done += st[i].reads + st[i].writes;
	}
	return !on || done >= VM_GUARD_PROVEN;
}

/* The worker, every look until the guard is settled: comes on once the home
 * screen is up, then writes '0' once the plugin has shown it works
 * (guard.h). Safe mode and an unknown firmware never get here. */
static void guard_tick(void)
{
	const uint32_t ms = uptime_ms();
	int shell = 1, frames = 1, proven = 0;

	if (g_guard_done)
		return;
	if (g_guard.phase == VM_GP_BOOT) {
		shell = shell_up();
		frames = shell > 0 ? shell_frames() : 0;
	} else {
		proven = guard_proven();
	}
	switch (vm_guard_step(&g_guard, shell, frames, proven)) {
	case VM_GA_GO:
		if (!g_guard_armed) {                 /* the '1' is not there: stay off */
			g_guard_done = 1;
			break;
		}
		if (g_disabled == VM_DIS_NONE && vm_stage_go())
			vm_settings_reapply();            /* the devices as set, now */
		g_on_ms = ms ? ms : 1;
		g_on = 1;
		VM_KLOGI("on at %u.%01us: %s", (unsigned)(ms / 1000u), (unsigned)(ms % 1000u / 100u),
		         k_guard_why[g_guard.why]);
		break;
	case VM_GA_CLEAR:
		VM_KLOGI("boot guard: the start is good at %u.%01us", (unsigned)(ms / 1000u),
		         (unsigned)(ms % 1000u / 100u));
		if (guard_write('0') != VM_OK)
			VM_KLOGW("boot guard: '0' not written - the next start will be safe mode");
		g_guard_done = 1;
		vm_klog_set_sync(0);              /* the start is past: lines as usual */
		break;
	default:
		break;
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

static void worker_round(VmDevState *prev);

static int worker(SceSize args, void *argp)
{
	VmDevState prev[VM_DEV_COUNT];

	(void)args; (void)argp;
	memset(prev, 0, sizeof(prev));
	while (g_run) {
		const int running = g_disabled == VM_DIS_NONE || g_disabled == VM_DIS_HOOKS;
		const int hud = running ? vm_hud_link_busy() : VM_HUD_IDLE;   /* hud_link.h */
		SceUInt wait = 0;

		if (running && !g_guard_done && g_guard.phase == VM_GP_BOOT)
			wait = VM_WORKER_BOOT_US;       /* waiting for the home screen */
		else if (hud == VM_HUD_MENU)
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
		__atomic_store_n(&g_worker_busy, 1, __ATOMIC_SEQ_CST);
		if (__atomic_load_n(&g_pw_down, __ATOMIC_SEQ_CST)) {   /* going down: nothing written */
			__atomic_store_n(&g_worker_busy, 0, __ATOMIC_RELEASE);
			continue;
		}
		worker_round(prev);
		__atomic_store_n(&g_worker_busy, 0, __ATOMIC_RELEASE);
	}
	return 0;
}

/* One round of the worker (g_worker_busy set). */
static void worker_round(VmDevState *prev)
{
	VmDevState now[VM_DEV_COUNT];

	g_uptime = uptime_s();
	/* Always, also in safe mode or on an unknown firmware: settings can
	 * still be changed there (to switch off what broke the last start),
	 * so they are saved, and the log follows its switch. */
	vm_settings_flush();
	vm_klog_set_enabled(__atomic_load_n(&g_vm_settings.log, __ATOMIC_RELAXED));   /* follows the switch */
	/* Safe mode stays until it is reset, an unknown firmware and a
	 * driver switched off never set the guard: nothing else runs in
	 * those three. */
	if (g_disabled == VM_DIS_SAFE || g_disabled == VM_DIS_FIRMWARE || g_disabled == VM_DIS_OFF)
		return;
	guard_tick();
	if (!g_on)
		return;                         /* the console still starts: nothing yet */
	vm_stage_worker_tick();
	vm_stage_get(now, 1);
	vm_hud_link_tick(now, worker_wake);
	vm_stage_io_tick();                 /* the overlay's files, from here: its stack free */
	for (int i = 0; i < VM_DEV_COUNT; i++) {
		int fell_back = now[i].status == VM_ST_FALLBACK && prev[i].status != VM_ST_FALLBACK;
		int new_err = now[i].errors != prev[i].errors ||
		              now[i].mismatches != prev[i].mismatches;

		if (fell_back || new_err)
			log_problem(&now[i], i);
		prev[i] = now[i];
	}
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

/* SceKernelModuleInfo as on 3.60-3.74 (0x1B8 bytes), the segments alone
 * named: the SDK this builds with may not declare it, nor
 * ksceKernelGetModuleInfoByAddr (SceModulemgrForDriver), looked up at run
 * time. */
typedef struct {
	uint32_t size;
	uint8_t  head[336];
	struct { uint32_t size, perms; void *vaddr; uint32_t memsz, filesz, res; } seg[4];
	uint32_t state;
} VmModInfo;
_Static_assert(sizeof(VmModInfo) == 0x1B8, "SceKernelModuleInfo is 0x1B8 bytes");

uint32_t vm_module_bytes(const void *addr)
{
	static int (*by_addr)(const void *a, VmModInfo *info);
	VmModInfo mi;
	uint32_t n = 0;

	if (!by_addr) {
		uintptr_t f = 0;

		if (module_get_export_func(KERNEL_PID, "SceKernelModulemgr", 0xD4A60A52, 0x1D9E0F7E,
		                           &f) < 0 || !f)
			return 0;
		by_addr = (int (*)(const void *, VmModInfo *))f;
	}
	memset(&mi, 0, sizeof(mi));
	mi.size = sizeof(mi);
	if (!addr || by_addr(addr, &mi) < 0)
		return 0;
	for (int i = 0; i < 4; i++)
		n += mi.seg[i].memsz;
	return n;
}

int vmGetInfo(VmInfo *out)
{
	static const char b[] = VM_BUILD_STR;
	VmDevState dev[VM_DEV_COUNT];
	VmInfo k;
	uint32_t hud_code;

	if (!out)
		return VM_ERR_ARG;
	memset(&k, 0, sizeof(k));
	k.size = sizeof(k);
	k.magic = VM_INFO_MAGIC;
	memcpy(k.build, b, sizeof(b) < sizeof(k.build) ? sizeof(b) : sizeof(k.build) - 1);
	k.disabled = g_disabled;
	k.fingerprint = g_fingerprint;
	k.uptime_s = uptime_s();
	k.on_ms = g_on_ms;
	vm_stage_info(&k);                 /* the memory, and what is free */
	k.threads += g_worker >= 0 ? VM_STACK_WORKER : 0;
	k.hud_loaded = vm_hud_link_mem(&k.hud, &hud_code);
	k.code = vm_module_bytes((const void *)vmGetInfo) + hud_code;
	vm_stage_get(dev, g_disabled == VM_DIS_NONE);
	for (int i = 0; i < VM_DEV_COUNT; i++)
		k.dev_status[i] = dev[i].status;
	k.memcard_slow = vm_stage_memcard_slow();
	if (ksceKernelMemcpyKernelToUser(out, &k, sizeof(k)) < 0)
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

	/* Switched off in Settings (Driver): this start installs nothing and
	 * writes nothing - not even the boot guard; the settings can still be
	 * changed, to switch it on for the next start. */
	if (!__atomic_load_n(&g_vm_settings.driver, __ATOMIC_RELAXED)) {
		g_disabled = VM_DIS_OFF;
		VM_KLOGI("switched off in Settings (Driver) - nothing installed");
		start_worker();
		return SCE_KERNEL_START_SUCCESS;
	}
	/* Crash-loop guard (guard.h): the file holds '1' from here until the
	 * plugin, on after the home screen came up, has shown it works, then
	 * '0'. Finding '1' means the last start never got that far: this one
	 * installs nothing; Settings > Advanced > Reset safe mode
	 * (vmResetSafeMode) clears it. */
	if (guard_read() == '1') {
		g_disabled = VM_DIS_SAFE;
		VM_KLOGW("boot_guard is 1 [%s] - safe mode, nothing installed (reset it in "
		         "Settings > Devices > vita-memio > Advanced)", g_guard_txt);
		start_worker();                   /* settings can still be changed */
		return SCE_KERNEL_START_SUCCESS;
	}
	g_guard_armed = vm_guard_arm(VM_GUARD_PATH, VM_DATA_DIR, &g_guard_len) == 0;
	if (!g_guard_armed)
		VM_KLOGW("boot guard: '1' could not be written and read back - the plugin stays "
		         "off this start");
	shell_lookup_init();
	frames_lookup_init();
	VM_KLOGI("start: boot guard %s at %u ms, the storage hooks next",
	         g_guard_armed ? "armed" : "NOT armed", (unsigned)uptime_ms());

	if (vm_stage_init() != VM_OK) {
		g_disabled = VM_DIS_HOOKS;
		VM_KLOGW("storage hooks NOT installed: storage stays stock");
	} else {
		power_hooks_init();
	}
	start_worker();
	VM_KLOGI("start: done at %u ms, every transfer the standard way until the home screen",
	         (unsigned)uptime_ms());
	return SCE_KERNEL_START_SUCCESS;
}

int module_stop(SceSize argc, const void *args)
{
	(void)argc; (void)args;
	/* Every state runs the worker; the rest releases only what was set up. */
	power_hooks_fini();
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
