/*
 * settings.c - load, validate, apply and save vita-memio's settings.
 *
 * The file is tiny and versioned:
 *   magic 'TSCF' | version | count | count x int32 (enum vm_key order) | check
 * Only this build's exact file is used: another version or key count, a
 * value out of range or a wrong check, and it is removed - every setting its
 * default (written again at the next change). A missing file: defaults.
 *
 * vm_settings_set() runs in the caller's syscall context, so it only updates
 * memory (and the staging switches) and marks the settings dirty; the
 * worker (main.c) does the file write from its own kernel thread.
 */
#include <psp2kern/kernel/sysclib.h>   /* memcpy/memset                        */
#include <psp2kern/kernel/threadmgr.h> /* ksceKernelDelayThread (the set lock)  */
#include <psp2kern/io/fcntl.h>
#include <psp2kern/io/stat.h>

#include "settings.h"
#include "klog.h"
#include "stage.h"

#define VM_CFG_MAGIC    0x46435354u   /* 'TSCF'                                */
#define VM_CFG_VERSION  3u            /* 3: Kernel memory Full / Medium / Low */

typedef struct VmSettingsFile {
	uint32_t magic;
	uint32_t version;
	uint32_t count;
	int32_t  values[VM_KEY_COUNT];
	uint32_t check;
} VmSettingsFile;

volatile VmSettings g_vm_settings;
/* Set (release) after a changed value is in g_vm_settings, taken (acquire)
 * by the worker before it reads them: a value is never saved stale with the
 * flag already cleared. */
static volatile int g_dirty = 0;
static volatile int g_save_fails = 0;   /* retry a failed save a few times */
/* One vm_settings_set at a time (they run in callers' syscalls): each applies
 * a snapshot that has every earlier change in it. */
static volatile int g_set_lock;

static const int32_t g_defaults[VM_KEY_COUNT] = {
	VM_DEF_SD2VITA, VM_DEF_INTERNAL, VM_DEF_VERIFY, VM_DEF_HUD, VM_DEF_HUD_STYLE,
	VM_DEF_LOG, VM_DEF_MEMCARD, VM_DEF_SMALL, VM_DEF_STATS, VM_DEF_GPUSAFE,
	VM_DEF_PIPELINE, VM_DEF_OFFLOAD, VM_DEF_READAHEAD, VM_DEF_OVERLAY, VM_DEF_DMACOPY,
	VM_DEF_DRIVER, VM_DEF_SYSAPPS, VM_DEF_LOWMEM,
};

/* Check word over the stored values. */
static uint32_t check_of(uint32_t magic, uint32_t version, uint32_t count, const int32_t *values)
{
	uint32_t c = magic ^ (version << 8) ^ (count << 16);
	for (uint32_t i = 0; i < count; i++)
		c = (c << 5 | c >> 27) ^ (uint32_t)values[i];
	return c;
}

static uint32_t file_check(const VmSettingsFile *f)
{
	return check_of(f->magic, f->version, f->count, f->values);
}

/* Read and check one settings file into `s` (which already holds the
 * defaults). 1 = loaded; 0 = missing, or not this build's exact file (then
 * removed, `s` untouched). */
static int load_file(const char *path, VmSettings *s)
{
	VmSettingsFile f;
	SceUID fd = ksceIoOpen(path, SCE_O_RDONLY, 0);
	int n, ok;

	if (fd < 0)
		return 0;                      /* no file: defaults, nothing to report */
	memset(&f, 0, sizeof(f));
	n = ksceIoRead(fd, &f, sizeof(f));
	ksceIoClose(fd);
	ok = n == (int)sizeof(f) && f.magic == VM_CFG_MAGIC && f.version == VM_CFG_VERSION &&
	     f.count == VM_KEY_COUNT && f.check == file_check(&f);
	for (int k = 0; k < VM_KEY_COUNT && ok; k++)
		ok = vm_setting_valid(k, f.values[k]);
	if (!ok) {
		VM_KLOGW("settings: %s not this build's (n=%d): removed, defaults", path, n);
		ksceIoRemove(path);
		return 0;
	}
	for (int k = 0; k < VM_KEY_COUNT; k++)
		*vm_setting_field(s, k) = f.values[k];
	return 1;
}

void vm_settings_init(void)
{
	VmSettings s;
	for (int k = 0; k < VM_KEY_COUNT; k++)
		*vm_setting_field(&s, k) = g_defaults[k];

	/* The main file first; if a save was cut short between removing the old file
	 * and renaming the new one, the finished copy is still in the .tmp file. */
	if (!load_file(VM_SETTINGS_PATH, &s))
		load_file(VM_SETTINGS_TMP, &s);
	memcpy((void *)&g_vm_settings, &s, sizeof(s));
	vm_stage_apply(&s);
}

int vm_settings_set(int key, int value)
{
	if (!vm_setting_valid(key, value))
		return VM_ERR_ARG;
	int32_t *f = vm_setting_field((VmSettings *)&g_vm_settings, key);
	if (!f)
		return VM_ERR_ARG;
	while (__sync_lock_test_and_set(&g_set_lock, 1))
		ksceKernelDelayThread(100);
	if (__atomic_load_n(f, __ATOMIC_RELAXED) != value) {
		VmSettings now;

		__atomic_store_n(f, value, __ATOMIC_RELAXED);
		__atomic_store_n(&g_save_fails, 0, __ATOMIC_RELAXED);
		vm_settings_copy(&now);
		vm_stage_apply(&now);              /* takes effect on the next transfer */
		__atomic_store_n(&g_dirty, 1, __ATOMIC_RELEASE);   /* after the value */
	}
	__sync_lock_release(&g_set_lock);
	return VM_OK;
}

/* The settings applied again (the plugin came on, vm_stage_go): under the
 * same lock as vm_settings_set, so one that applies at the same time, still
 * with the devices held, cannot leave them off. */
void vm_settings_reapply(void)
{
	VmSettings now;

	while (__sync_lock_test_and_set(&g_set_lock, 1))
		ksceKernelDelayThread(100);
	vm_settings_copy(&now);
	vm_stage_apply(&now);
	__sync_lock_release(&g_set_lock);
}

/* The console resets or powers off (1: vm_stage_down; the settings saved
 * now, not by the worker meanwhile), or that failed (0: the settings applied
 * again). Under the same lock as vm_settings_set, so none applied at the
 * same time switches a storage back on. 1 if quiet in time. */
int vm_settings_down(int down)
{
	VmSettings now;
	int r;

	while (__sync_lock_test_and_set(&g_set_lock, 1))
		ksceKernelDelayThread(100);
	r = vm_stage_down(down);
	if (!down) {
		vm_settings_copy(&now);
		vm_stage_apply(&now);
	}
	__sync_lock_release(&g_set_lock);
	if (down)
		vm_settings_flush();
	return r;
}

void vm_settings_copy(VmSettings *out)
{
	/* word by word, each read whole (a key may be set meanwhile) */
	for (int k = 0; k < VM_KEY_COUNT; k++)
		*vm_setting_field(out, k) =
			__atomic_load_n(vm_setting_field((VmSettings *)&g_vm_settings, k), __ATOMIC_RELAXED);
}

int vm_settings_pending(void)
{
	return __atomic_load_n(&g_dirty, __ATOMIC_ACQUIRE);
}

void vm_settings_flush(void)
{
	if (!__atomic_exchange_n(&g_dirty, 0, __ATOMIC_ACQ_REL))
		return;

	VmSettingsFile f;
	VmSettings s;
	vm_settings_copy(&s);
	memset(&f, 0, sizeof(f));
	f.magic = VM_CFG_MAGIC;
	f.version = VM_CFG_VERSION;
	f.count = VM_KEY_COUNT;
	for (int k = 0; k < VM_KEY_COUNT; k++)
		f.values[k] = *vm_setting_field(&s, k);
	f.check = file_check(&f);

	ksceIoMkdir("ur0:data", 0777);
	ksceIoMkdir(VM_DATA_DIR, 0777);
	/* Write the new copy aside, then swap it in. The live file is never left
	 * truncated: at any moment either it or the finished .tmp copy is intact. */
	SceUID fd = ksceIoOpen(VM_SETTINGS_TMP,
	                       SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
	int w = fd >= 0 ? ksceIoWrite(fd, &f, sizeof(f)) : fd;
	if (fd >= 0)
		ksceIoClose(fd);
	if (w == (int)sizeof(f)) {
		ksceIoRemove(VM_SETTINGS_PATH);
		int r = ksceIoRename(VM_SETTINGS_TMP, VM_SETTINGS_PATH);
		if (r < 0)
			w = r;      /* the .tmp copy stays and is loaded at the next boot */
	}
	if (w != (int)sizeof(f)) {
		if (__atomic_fetch_add(&g_save_fails, 1, __ATOMIC_RELAXED) < 5) {
			VM_KLOGW("settings: save failed (0x%08x), will retry", (unsigned)w);
			__atomic_store_n(&g_dirty, 1, __ATOMIC_RELEASE);
		} else {
			VM_KLOGE("settings: save failed (0x%08x), giving up until the next change",
			         (unsigned)w);
		}
		return;
	}
	__atomic_store_n(&g_save_fails, 0, __ATOMIC_RELAXED);
}
