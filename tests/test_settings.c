/*
 * test_settings.c - host tests for kernel/settings.c: load, validate, save, and
 * what is left behind when a save is cut short. The file system is a temp
 * directory (see fakes.c); its rename does not replace an existing file, like the
 * real one.
 */
#include "../kernel/settings.c"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "fakes.h"
#include "vm_test.h"

/* settings.c hands its result to the storage code; record it instead. */
static int apply_calls;
static VmSettings last_applied;

void vm_stage_apply(const VmSettings *s)
{
	apply_calls++;
	last_applied = *s;
}

static int down_calls, down_last = -1;

int vm_stage_down(int down)
{
	down_calls++;
	down_last = down;
	return 1;
}

static const char *dir;

static void path_of(const char *name, char *out, size_t n)
{
	snprintf(out, n, "%s/%s", dir, name);
}

static int exists(const char *name)
{
	char p[300];

	path_of(name, p, sizeof(p));
	return access(p, F_OK) == 0;
}

static void put_file(const char *name, const void *data, size_t n)
{
	char p[300];

	path_of(name, p, sizeof(p));
	FILE *f = fopen(p, "wb");
	fwrite(data, 1, n, f);
	fclose(f);
}

static size_t get_file(const char *name, void *out, size_t cap)
{
	char p[300];

	path_of(name, p, sizeof(p));
	FILE *f = fopen(p, "rb");
	if (!f)
		return 0;
	size_t n = fread(out, 1, cap, f);
	fclose(f);
	return n;
}

static void clean(void)
{
	char p[300];
	const char *names[] = { "settings.bin", "settings.tmp" };

	for (int i = 0; i < 2; i++) {
		path_of(names[i], p, sizeof(p));
		unlink(p);
	}
	fakes_reset();
	apply_calls = 0;
	g_dirty = 0;
	g_save_fails = 0;
}

static int all_defaults(const VmSettings *s)
{
	return s->sd2vita == VM_DEF_SD2VITA && s->internal == VM_DEF_INTERNAL &&
	       s->verify == VM_DEF_VERIFY && s->hud == VM_DEF_HUD &&
	       s->hud_style == VM_DEF_HUD_STYLE && s->log == VM_DEF_LOG &&
	       s->memcard == VM_DEF_MEMCARD && s->small == VM_DEF_SMALL &&
	       s->stats == VM_DEF_STATS && s->gpusafe == VM_DEF_GPUSAFE &&
	       s->pipeline == VM_DEF_PIPELINE;
}

/* Build a settings file by hand: this build's layout and size, the given
 * version and count, a correct check word. */
static size_t make_file_v(uint8_t *out, uint32_t count, const int32_t *vals, uint32_t version)
{
	VmSettingsFile f;

	memset(&f, 0, sizeof(f));
	f.magic = VM_CFG_MAGIC;
	f.version = version;
	f.count = count;
	for (uint32_t i = 0; i < count && i < VM_KEY_COUNT; i++)
		f.values[i] = vals[i];
	f.check = check_of(f.magic, f.version, count < VM_KEY_COUNT ? count : VM_KEY_COUNT, f.values);
	memcpy(out, &f, sizeof(f));
	return sizeof(f);
}

/* Every key at its default but the ones the test sets. */
static void defaults_into(int32_t *v)
{
	for (int k = 0; k < VM_KEY_COUNT; k++)
		v[k] = g_defaults[k];
}

/* The file is not this build's: every setting its default, the file gone
 * (and a change saves this build's file again). */
static void check_refused(const uint8_t *buf, size_t n)
{
	VmSettings s;

	clean();
	put_file("settings.bin", buf, n);
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(all_defaults(&s) && s.lowmem == VM_DEF_LOWMEM && s.offload == VM_DEF_OFFLOAD);
	CHECK(!exists("settings.bin") && !exists("settings.tmp"));
	CHECK(vm_settings_set(VM_KEY_SD2VITA, 1) == VM_OK);
	vm_settings_flush();
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(s.sd2vita == 1 && exists("settings.bin"));
}

static void test_no_file_gives_defaults(void)
{
	VmSettings s;

	clean();
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(all_defaults(&s));
	CHECK(apply_calls == 1 && all_defaults(&last_applied));
}

static void test_save_and_load_roundtrip(void)
{
	VmSettings s;

	clean();
	vm_settings_init();
	CHECK(vm_settings_set(VM_KEY_SD2VITA, 1) == VM_OK);
	CHECK(vm_settings_set(VM_KEY_HUD, 2) == VM_OK);
	CHECK(vm_settings_set(VM_KEY_HUD_STYLE, VM_HUD_STYLE_CLASSIC) == VM_OK);
	CHECK(vm_settings_set(VM_KEY_LOG, 1) == VM_OK);
	CHECK(vm_settings_set(VM_KEY_MEMCARD, 1) == VM_OK);
	CHECK(vm_settings_set(VM_KEY_SMALL, 1) == VM_OK);
	CHECK(last_applied.sd2vita == 1 && last_applied.memcard == 1 && last_applied.small == 1);
	vm_settings_flush();
	CHECK(exists("settings.bin") && !exists("settings.tmp"));

	vm_settings_init();                       /* "reboot" */
	vm_settings_copy(&s);
	CHECK(s.sd2vita == 1 && s.hud == 2 && s.hud_style == VM_HUD_STYLE_CLASSIC && s.memcard == 1);
	CHECK(s.log == 1 && s.small == 1 && s.internal == 0 && s.verify == 0);
}

static void test_invalid_set_is_rejected(void)
{
	VmSettings s;

	clean();
	vm_settings_init();
	apply_calls = 0;
	CHECK(vm_settings_set(VM_KEY_SD2VITA, 2) == VM_ERR_ARG);
	CHECK(vm_settings_set(VM_KEY_HUD, -1) == VM_ERR_ARG);
	CHECK(vm_settings_set(VM_KEY_HUD, VM_HUD_OFF) == VM_ERR_ARG);   /* only Extended */
	CHECK(vm_settings_set(VM_KEY_HUD, VM_HUD_COMPACT) == VM_ERR_ARG);
	CHECK(vm_settings_set(VM_KEY_HUD_STYLE, 2) == VM_ERR_ARG);
	CHECK(vm_settings_set(99, 0) == VM_ERR_ARG);
	CHECK(vm_settings_set(-1, 0) == VM_ERR_ARG);
	CHECK(vm_settings_set(VM_KEY_LOG, 2) == VM_ERR_ARG);
	vm_settings_copy(&s);
	CHECK(all_defaults(&s) && apply_calls == 0 && !g_dirty);
	CHECK(vm_settings_set(VM_KEY_SD2VITA, 0) == VM_OK);   /* same value: nothing to do */
	CHECK(apply_calls == 0 && !g_dirty);
}

static void test_corrupt_file_gives_defaults(void)
{
	uint8_t buf[128];
	VmSettings s;
	size_t n;

	clean();
	vm_settings_init();
	vm_settings_set(VM_KEY_VERIFY, 1);
	vm_settings_flush();
	n = get_file("settings.bin", buf, sizeof(buf));
	CHECK(n == sizeof(VmSettingsFile));

	buf[14] ^= 0x01;                          /* flip a bit in a value */
	put_file("settings.bin", buf, n);
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(all_defaults(&s));                  /* checksum catches it: all off */

	put_file("settings.bin", buf, 20);        /* truncated */
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(all_defaults(&s));

	put_file("settings.bin", "", 0);          /* empty (the old non-atomic save) */
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(all_defaults(&s));
}

/* Value rules of the newer keys; a full file keeps every stored value. */
static void test_values_and_full_file(void)
{
	uint8_t buf[256];
	int32_t v[VM_KEY_COUNT];
	VmSettings s;
	size_t n;

	clean();
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(s.driver == 1 && VM_DEF_DRIVER == 1);
	CHECK(s.lowmem == VM_KMEM_MEDIUM && VM_DEF_LOWMEM == VM_KMEM_MEDIUM);
	CHECK(VM_DEF_GPUSAFE == VM_GPUSAFE_NODMA && VM_DEF_PIPELINE == VM_PIPE_AUTO);
	CHECK(VM_DEF_OFFLOAD == VM_OFFLOAD_ALL && VM_KEY_COUNT == VM_KEY_LOWMEM + 1);
	CHECK(vm_settings_set(VM_KEY_DRIVER, 2) == VM_ERR_ARG);
	CHECK(vm_settings_set(VM_KEY_SYSAPPS, 2) == VM_ERR_ARG);
	CHECK(vm_settings_set(VM_KEY_LOWMEM, 3) == VM_ERR_ARG && vm_settings_set(VM_KEY_LOWMEM, -1) == VM_ERR_ARG);
	CHECK(vm_settings_set(VM_KEY_LOWMEM, VM_KMEM_FULL) == VM_OK &&
	      vm_settings_set(VM_KEY_LOWMEM, VM_KMEM_LOW) == VM_OK &&
	      vm_settings_set(VM_KEY_LOWMEM, VM_KMEM_MEDIUM) == VM_OK);
	CHECK(vm_settings_set(VM_KEY_GPUSAFE, 3) == VM_ERR_ARG && vm_settings_set(VM_KEY_PIPELINE, 3) == VM_ERR_ARG);
	CHECK(vm_setting_valid(VM_KEY_OFFLOAD, VM_OFFLOAD_ALL) && !vm_setting_valid(VM_KEY_OFFLOAD, 1) &&
	      vm_setting_valid(VM_KEY_OFFLOAD, VM_OFFLOAD_ALWAYS) && vm_setting_valid(VM_KEY_OFFLOAD, VM_OFFLOAD_OFF) &&
	      !vm_setting_valid(VM_KEY_OFFLOAD, 4) && !vm_setting_valid(VM_KEY_OFFLOAD, -1));
	CHECK(vm_setting_valid(VM_KEY_STATS, VM_STATS_OFF) && vm_setting_valid(VM_KEY_STATS, VM_STATS_ON) &&
	      !vm_setting_valid(VM_KEY_STATS, 2));
	CHECK(vm_settings_set(VM_KEY_INTERNAL, 1) == VM_ERR_ARG && vm_settings_set(VM_KEY_INTERNAL, 0) == VM_OK);

	defaults_into(v);
	v[VM_KEY_SD2VITA] = 1;
	v[VM_KEY_GPUSAFE] = 0;
	v[VM_KEY_PIPELINE] = VM_PIPE_FIXED;
	v[VM_KEY_OFFLOAD] = VM_OFFLOAD_ALWAYS;
	v[VM_KEY_DRIVER] = 0;
	v[VM_KEY_LOWMEM] = VM_KMEM_LOW;
	clean();
	n = make_file_v(buf, VM_KEY_COUNT, v, VM_CFG_VERSION);
	put_file("settings.bin", buf, n);
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(s.sd2vita == 1 && s.gpusafe == 0 && s.pipeline == VM_PIPE_FIXED);
	CHECK(s.offload == VM_OFFLOAD_ALWAYS && s.driver == 0 && s.lowmem == VM_KMEM_LOW);
	CHECK(exists("settings.bin"));
}

/* The plugin comes on (main.c): the settings in effect applied again, as
 * they are, nothing saved for it. */
static void test_reapply(void)
{
	VmSettings s;
	int calls;

	clean();
	vm_settings_init();
	CHECK(vm_settings_set(VM_KEY_SD2VITA, 1) == VM_OK);
	vm_settings_flush();
	calls = apply_calls;
	memset(&last_applied, 0, sizeof(last_applied));
	vm_settings_reapply();
	vm_settings_copy(&s);
	CHECK(apply_calls == calls + 1);
	CHECK(!memcmp(&last_applied, &s, sizeof(s)) && s.sd2vita == 1);
	CHECK(!vm_settings_pending());
}

/* A save to do keeps the worker waking: pending after a change, not after a
 * save, pending again while a failed save is retried, not once it gives up. */
static void test_pending(void)
{
	clean();
	vm_settings_init();
	vm_settings_flush();
	CHECK(!vm_settings_pending());
	CHECK(vm_settings_set(VM_KEY_SD2VITA, 1) == VM_OK && vm_settings_pending());
	vm_settings_flush();
	CHECK(!vm_settings_pending());
	CHECK(vm_settings_set(VM_KEY_SD2VITA, 1) == VM_OK && !vm_settings_pending());   /* no change */
	fake_write_short = 1;
	CHECK(vm_settings_set(VM_KEY_SD2VITA, 0) == VM_OK);
	for (int i = 0; i < 5; i++) {
		vm_settings_flush();
		CHECK(vm_settings_pending());                /* retried, five times */
	}
	vm_settings_flush();
	CHECK(!vm_settings_pending());                   /* gave up until the next change */
	fake_write_short = 0;
}

/* Not this build's file - an older or newer build's key count or version,
 * one value out of range, a wrong check, a wrong size: no compatibility,
 * the file goes and every setting takes its default. */
static void test_mismatch_resets_to_defaults(void)
{
	uint8_t buf[256];
	int32_t v[VM_KEY_COUNT];
	size_t n;

	defaults_into(v);
	v[VM_KEY_SD2VITA] = 1;
	v[VM_KEY_MEMCARD] = 1;
	v[VM_KEY_LOWMEM] = VM_KMEM_FULL;

	n = make_file_v(buf, VM_KEY_COUNT - 3, v, VM_CFG_VERSION);   /* fewer keys */
	check_refused(buf, n);
	n = make_file_v(buf, VM_KEY_COUNT + 1, v, VM_CFG_VERSION);   /* more keys */
	check_refused(buf, n);
	n = make_file_v(buf, VM_KEY_COUNT, v, VM_CFG_VERSION - 1);   /* the older version */
	check_refused(buf, n);
	n = make_file_v(buf, VM_KEY_COUNT, v, VM_CFG_VERSION);
	check_refused(buf, n - 4);                                   /* short */
	buf[n - 1] ^= 1;                                             /* wrong check */
	check_refused(buf, n);

	static const int bad[][2] = {
		{ VM_KEY_HUD, VM_HUD_OFF }, { VM_KEY_HUD_STYLE, 3 }, { VM_KEY_INTERNAL, 1 },
		{ VM_KEY_OFFLOAD, 4 }, { VM_KEY_PIPELINE, 3 }, { VM_KEY_LOWMEM, 3 },
	};
	for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		int32_t w[VM_KEY_COUNT];

		memcpy(w, v, sizeof(w));
		w[bad[i][0]] = bad[i][1];
		n = make_file_v(buf, VM_KEY_COUNT, w, VM_CFG_VERSION);
		check_refused(buf, n);
	}
}

/* A bad .tmp copy (no live file) is removed too. */
static void test_bad_tmp_removed(void)
{
	uint8_t buf[256];
	int32_t v[VM_KEY_COUNT];
	VmSettings s;
	size_t n;

	defaults_into(v);
	v[VM_KEY_SD2VITA] = 1;
	n = make_file_v(buf, VM_KEY_COUNT, v, VM_CFG_VERSION - 1);
	clean();
	put_file("settings.tmp", buf, n);
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(all_defaults(&s) && !exists("settings.tmp") && !exists("settings.bin"));
}

static void test_cut_save_after_remove_loads_tmp(void)
{
	VmSettings s;

	clean();
	vm_settings_init();
	vm_settings_set(VM_KEY_SD2VITA, 1);
	vm_settings_flush();

	/* second save: the old file is removed, then the rename fails (power cut) */
	vm_settings_set(VM_KEY_MEMCARD, 1);
	fake_rename_fail = 1;
	vm_settings_flush();
	CHECK(!exists("settings.bin") && exists("settings.tmp"));
	CHECK(g_dirty == 1);                      /* will be retried */

	fake_rename_fail = 0;
	vm_settings_init();                       /* boot: no main file, finished .tmp copy */
	vm_settings_copy(&s);
	CHECK(s.sd2vita == 1 && s.memcard == 1);
}

static void test_short_write_leaves_old_file(void)
{
	VmSettings s;

	clean();
	vm_settings_init();
	vm_settings_set(VM_KEY_SD2VITA, 1);
	vm_settings_flush();                      /* good copy A on disk */

	vm_settings_set(VM_KEY_VERIFY, 1);
	fake_write_short = 1;                     /* the next save is cut short */
	vm_settings_flush();
	CHECK(exists("settings.bin"));            /* the old file was never touched */
	CHECK(g_dirty == 1);

	fake_write_short = 0;
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(s.sd2vita == 1 && s.verify == 0);   /* A, not a torn mix */
}

static void test_save_gives_up_after_retries(void)
{
	clean();
	vm_settings_init();
	vm_settings_set(VM_KEY_SD2VITA, 1);
	fake_write_short = 1;
	for (int i = 0; i < 10; i++)
		vm_settings_flush();
	CHECK(g_dirty == 0);                      /* stopped retrying (and logged) */
	vm_settings_set(VM_KEY_MEMCARD, 1);       /* the next change tries again */
	CHECK(g_dirty == 1);
}

/* A reset or power off: the storage code told, a change waiting is saved
 * now; refused: the settings applied again, as they are. */
static void test_down_saves_and_reapplies(void)
{
	VmSettings s;

	clean();
	vm_settings_init();
	CHECK(vm_settings_set(VM_KEY_SD2VITA, 1) == VM_OK && vm_settings_pending());
	down_calls = 0;
	CHECK(vm_settings_down(1) == 1 && down_calls == 1 && down_last == 1);
	CHECK(!vm_settings_pending() && exists("settings.bin"));
	apply_calls = 0;
	CHECK(vm_settings_down(0) == 1 && down_last == 0 && apply_calls == 1);
	vm_settings_copy(&s);
	CHECK(!memcmp(&last_applied, &s, sizeof(s)) && s.sd2vita == 1);
	CHECK(!vm_settings_pending());
}

int main(void)
{
	dir = fakes_tmpdir_new();

	RUN(test_no_file_gives_defaults);
	RUN(test_save_and_load_roundtrip);
	RUN(test_invalid_set_is_rejected);
	RUN(test_corrupt_file_gives_defaults);
	RUN(test_values_and_full_file);
	RUN(test_reapply);
	RUN(test_pending);
	RUN(test_mismatch_resets_to_defaults);
	RUN(test_bad_tmp_removed);
	RUN(test_down_saves_and_reapplies);
	RUN(test_cut_save_after_remove_loads_tmp);
	RUN(test_short_write_leaves_old_file);
	RUN(test_save_gives_up_after_retries);

	fakes_tmpdir_remove();
	printf(vm_failures ? "test_settings: %d FAILED\n" : "test_settings: ok\n", vm_failures);
	return vm_failures ? 1 : 0;
}
