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
	       s->memcard == VM_DEF_MEMCARD;
}

/* Build a settings file by hand, with a correct check word. */
static size_t make_file_v(uint8_t *out, uint32_t count, const int32_t *vals, uint32_t version)
{
	VmSettingsFile f;
	size_t n;

	memset(&f, 0, sizeof(f));
	f.magic = VM_CFG_MAGIC;
	f.version = version;
	f.count = count;
	for (uint32_t i = 0; i < count; i++)
		f.values[i] = vals[i];
	uint32_t check = file_check(&f);

	/* the check word follows the last value actually stored */
	memcpy(out, &f, 3 * sizeof(uint32_t));
	memcpy(out + 12, f.values, count * sizeof(int32_t));
	memcpy(out + 12 + count * 4, &check, 4);
	n = 12 + count * 4 + 4;
	return n;
}

static size_t make_file(uint8_t *out, uint32_t count, const int32_t *vals)
{
	return make_file_v(out, count, vals, VM_CFG_VERSION);
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
	CHECK(last_applied.sd2vita == 1 && last_applied.memcard == 1);
	vm_settings_flush();
	CHECK(exists("settings.bin") && !exists("settings.tmp"));

	vm_settings_init();                       /* "reboot" */
	vm_settings_copy(&s);
	CHECK(s.sd2vita == 1 && s.hud == 2 && s.hud_style == VM_HUD_STYLE_CLASSIC && s.memcard == 1);
	CHECK(s.log == 1 && s.internal == 0 && s.verify == 0);
}

static void test_invalid_set_is_rejected(void)
{
	VmSettings s;

	clean();
	vm_settings_init();
	apply_calls = 0;
	CHECK(vm_settings_set(VM_KEY_SD2VITA, 2) == VM_ERR_ARG);
	CHECK(vm_settings_set(VM_KEY_HUD, -1) == VM_ERR_ARG);
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
	uint8_t buf[64];
	VmSettings s;
	size_t n;

	clean();
	vm_settings_init();
	vm_settings_set(VM_KEY_INTERNAL, 1);
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

static void test_older_file_with_fewer_keys(void)
{
	uint8_t buf[64];
	int32_t v[6] = { 1, 0, 0, 2, 3, 0 };      /* 6 keys: no memcard yet */
	VmSettings s;
	size_t n;

	clean();
	n = make_file(buf, 6, v);
	put_file("settings.bin", buf, n);
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(s.sd2vita == 1 && s.hud == 2);
	CHECK(s.hud_style == VM_DEF_HUD_STYLE);   /* 3: out of range, the default */
	CHECK(s.memcard == VM_DEF_MEMCARD);       /* the newer key takes its default */
}

/* A file with more keys than this build knows (the experimental build wrote 8):
 * the check covers all of them, the known ones load, the rest is ignored. */
static void test_file_with_more_keys(void)
{
	uint8_t buf[256];
	int32_t v[8] = { 1, 0, 0, 2, 0, 1, 1, 1 };
	uint32_t hdr[3] = { VM_CFG_MAGIC, VM_CFG_VERSION, 8 }, check;
	VmSettings s;
	size_t n;

	clean();
	check = check_of(hdr[0], hdr[1], 8, v);
	memcpy(buf, hdr, 12);
	memcpy(buf + 12, v, sizeof(v));
	memcpy(buf + 12 + sizeof(v), &check, 4);
	n = 12 + sizeof(v) + 4;
	put_file("settings.bin", buf, n);
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(s.sd2vita == 1 && s.hud == 2 && s.hud_style == 0 && s.log == 1 && s.memcard == 1);

	/* the same file with a wrong check word is refused */
	buf[n - 1] ^= 1;
	put_file("settings.bin", buf, n);
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(s.sd2vita == VM_DEF_SD2VITA && s.memcard == VM_DEF_MEMCARD);
}

static void test_one_bad_value_keeps_the_rest(void)
{
	uint8_t buf[64];
	int32_t v[7] = { 1, 0, 0, 9 /* hud: out of range */, 0, 0, 1 };
	VmSettings s;
	size_t n;

	clean();
	n = make_file(buf, 7, v);
	put_file("settings.bin", buf, n);
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(s.hud == VM_DEF_HUD);
	CHECK(s.sd2vita == 1 && s.hud_style == 0 && s.memcard == 1);
}

/* A file of another version (1, before slots 4 and 5 changed meaning) is
 * not used. */
static void test_other_version_gives_defaults(void)
{
	uint8_t buf[64];
	const int32_t v[7] = { 1, 0, 0, 2, 0, 1, 1 };
	VmSettings s;
	size_t n;

	clean();
	n = make_file_v(buf, 7, v, 1);
	put_file("settings.bin", buf, n);
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(all_defaults(&s));
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

	vm_settings_set(VM_KEY_INTERNAL, 1);
	fake_write_short = 1;                     /* the next save is cut short */
	vm_settings_flush();
	CHECK(exists("settings.bin"));            /* the old file was never touched */
	CHECK(g_dirty == 1);

	fake_write_short = 0;
	vm_settings_init();
	vm_settings_copy(&s);
	CHECK(s.sd2vita == 1 && s.internal == 0); /* A, not a torn mix */
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
	vm_settings_set(VM_KEY_HUD, 1);           /* the next change tries again */
	CHECK(g_dirty == 1);
}

int main(void)
{
	dir = fakes_tmpdir_new();

	RUN(test_no_file_gives_defaults);
	RUN(test_save_and_load_roundtrip);
	RUN(test_invalid_set_is_rejected);
	RUN(test_corrupt_file_gives_defaults);
	RUN(test_older_file_with_fewer_keys);
	RUN(test_file_with_more_keys);
	RUN(test_one_bad_value_keeps_the_rest);
	RUN(test_other_version_gives_defaults);
	RUN(test_cut_save_after_remove_loads_tmp);
	RUN(test_short_write_leaves_old_file);
	RUN(test_save_gives_up_after_retries);

	fakes_tmpdir_remove();
	printf(vm_failures ? "test_settings: %d FAILED\n" : "test_settings: ok\n", vm_failures);
	return vm_failures ? 1 : 0;
}
